#include "rv/transform/bosccTransform.h"

#include <vector>
#include <sstream>

#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/LoopInfo.h>

#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/PostOrderIterator.h>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include "rv/vectorizationInfo.h"
#include "rv/PlatformInfo.h"

#include <rvConfig.h>
#include "report.h"

using namespace rv;
using namespace llvm;

#if 1
#define IF_DEBUG_BOSCC IF_DEBUG
#else
#define IF_DEBUG_BOSCC if (false)
#endif

struct Impl {
  VectorizationInfo & vecInfo;
  PlatformInfo & platInfo;
  DominatorTree & domTree;
  PostDominatorTree & postDomTree;
  LoopInfo & loopInfo;
  Module & mod;


Impl(VectorizationInfo & _vecInfo, PlatformInfo & _platInfo, DominatorTree & _domTree, PostDominatorTree & _postDomTree, LoopInfo & _loopInfo)
: vecInfo(_vecInfo)
, platInfo(_platInfo)
, domTree(_domTree)
, postDomTree(_postDomTree)
, loopInfo(_loopInfo)
, mod(*vecInfo.getScalarFunction().getParent())
{}

Function &
requestMaskIntrinsic(std::string name) {
  return *platInfo.requestMaskReductionFunc("rv_any");
}

void
transformBranch(BranchInst & branch, int succIdx) {
  auto & context = branch.getContext();
  assert(0 <= succIdx && succIdx <= 1);
  assert(branch.isConditional());
  assert(!vecInfo.getVectorShape(branch).isUniform());

  auto * succBlock = branch.getSuccessor(succIdx);
  auto * exitBlock = branch.getSuccessor(1 - succIdx);
  auto * branchCond = branch.getCondition();
  bool branchOnTrue = succIdx == 0;


  assert(domTree.dominates(branch.getParent(), succBlock) && "can only BOSCC over dominated parts for now");

// create a BOSCC condition block
  std::stringstream blockName;
  blockName << succBlock->getName().str() << "_boscc";
  auto * bosccBlock = BasicBlock::Create(context, blockName.str(), &vecInfo.getScalarFunction());

  // register with domTree
  auto * bosccNode = domTree.addNewBlock(bosccBlock, branch.getParent());
  domTree.changeImmediateDominator(domTree.getNode(succBlock), bosccNode);
  // register with loopInfo
  auto * succLoop = loopInfo.getLoopFor(succBlock);
  if (succLoop) succLoop->addBasicBlockToLoop(bosccBlock, loopInfo);
  // bosccBlock has same predicate as BOSCC successor
  vecInfo.setPredicate(*bosccBlock, *vecInfo.getPredicate(*succBlock));

  IRBuilder<> builder(bosccBlock);

// flip the condition (if need be)
  auto * bosccMask = branchCond;
  if (!branchOnTrue) {
    bosccMask = builder.CreateNot(branchCond, "neg");
    vecInfo.setVectorShape(*bosccMask, vecInfo.getVectorShape(*branchCond));
  }

// create BOSCC condition check
  auto & anyFunc = requestMaskIntrinsic("rv_any");
  auto * bosccCond = builder.CreateCall(&anyFunc, bosccMask, "boscc_test");
  vecInfo.setVectorShape(*bosccCond, VectorShape::uni());

// create the BOSCC branch
  auto * bosccBr = builder.CreateCondBr(bosccCond, succBlock, exitBlock);
  vecInfo.setVectorShape(*bosccBr, VectorShape::uni());

// link bosccBranch into old branch
   branch.setSuccessor(succIdx, bosccBlock);

// patch phis in succBlock
  for (auto & inst : *succBlock) {
    auto * phi =  dyn_cast<PHINode>(&inst);
    if (!phi) break;
    int branchBlockIdx = phi->getBasicBlockIndex(branch.getParent());
    phi->setIncomingBlock(branchBlockIdx, bosccBlock);
  }

// fix up PHI nodes in the exit block (we have a new incoming branch from the bosccBlock)
  for (auto & inst : *exitBlock) {
    auto * phi =  dyn_cast<PHINode>(&inst);
    if (!phi) break;
    phi->addIncoming(UndefValue::get(phi->getType()), bosccBlock);
  }
}

size_t
getDomRegionValue(BasicBlock & entry, BasicBlock & subBlock, SmallSet<BasicBlock*, 32> & seenBlocks) {
  if (&entry != &subBlock && !domTree.dominates(&entry, &subBlock)) return 0; // not in our dominance region
  if (!seenBlocks.insert(&entry).second) return 0; // already factores in

  // compute own score
  size_t score = getBlockScore(subBlock);

  // compute score of other (dominated) reachable blocks in the region
  auto * termInst = subBlock.getTerminator();
  for (int i = 0; i < termInst->getNumSuccessors(); ++i) {
    auto & nextBlock = *termInst->getSuccessor(i);
    score += getDomRegionValue(entry, nextBlock, seenBlocks);
  }
  return score;
}

size_t
getBlockScore(BasicBlock & entry) {
  size_t score = 0;

  for (auto & inst : entry) {
    auto * store = dyn_cast<StoreInst>(&inst);
    auto * load = dyn_cast<LoadInst>(&inst);
    auto * call = dyn_cast<CallInst>(&inst);

    Value * ptrOperand = nullptr;
    if (load) ptrOperand = load->getPointerOperand();
    else if (store) ptrOperand = store->getPointerOperand();

    if (ptrOperand) {
      if (vecInfo.getVectorShape(*ptrOperand).isVarying()) {
        score += 8;
      } else if (vecInfo.getVectorShape(*ptrOperand).isUniform()) {
        score += 1;
      } else { // strided
        score += 2;
      }
      continue;
    }

    if (call) {
      auto * callee = call->getCalledFunction();
      if (!callee) { score += 8; continue; }

      if (!platInfo.isFunctionVectorizable(callee->getName(), vecInfo.getVectorWidth())) {
        score += 2;
      }
      score += 1;
      continue;
    }

    score += 1;
  }

  return score;
}

size_t
getDomRegionScore(BasicBlock & entry) {
  SmallSet<BasicBlock*, 32> seenBlocks;
  return getDomRegionValue(entry, entry, seenBlocks);
}

int
GetNumPredecessors(BasicBlock & block) {
  int numPred = 0;
  for (auto * user : block.users()) {
    if (isa<TerminatorInst>(user)) {
      ++numPred;
    }
  }
  return numPred;
}

static int
GetValue(const char * name, int defVal) {
  auto * text = getenv(name);
  if (!text) return defVal;
  else return atoi(text);
}
// 0  : do not BOSCC
// -1 : boscc onTrue
// 1 : boscc onFalse
int
bosccHeuristic(BranchInst & branch, size_t & regScore) {
// run legality checks
  auto * onTrueBlock = branch.getSuccessor(0);
  auto * onFalseBlock = branch.getSuccessor(1);

  if (!vecInfo.inRegion(*onTrueBlock) || !vecInfo.inRegion(*onFalseBlock)) return 0;

  auto * branchLoop = loopInfo.getLoopFor(branch.getParent());
  auto * onTrueLoop = loopInfo.getLoopFor(onTrueBlock);
  auto * onFalseLoop = loopInfo.getLoopFor(onFalseBlock);

// don't speculate over loop exits for now (TODO)
  if (onTrueLoop != branchLoop || branchLoop != onFalseLoop) return 0;
  if (onTrueLoop && onTrueLoop->getHeader() == onTrueBlock) return 0;
  if (onFalseLoop && onFalseLoop->getHeader() == onTrueBlock) return 0;


// per sucess legality
  // legality checks for speculating over onTrue
  bool onTrueLegal = GetNumPredecessors(*onTrueBlock) == 1; //domTree.dominates(branch.getParent(), onTrueBlock);
  onTrueLegal &= !onTrueLoop || onTrueLoop->getLoopLatch() != onTrueBlock;

  // legality checks for speculating over onTrue
  bool onFalseLegal = GetNumPredecessors(*onFalseBlock) == 1; //domTree.dominates(branch.getParent(), onFalseBlock);
  onFalseLegal &= !onFalseLoop || onFalseLoop->getLoopLatch() != onFalseBlock;


// score the dominates parts of either branch target
  // only accept regions that are post dominated by the oposing branch
  size_t onTrueScore = 0;
  if (onTrueLegal) { // && postDomTree.dominates(onFalseBlock, onTrueBlock)) {
    onTrueScore = getDomRegionScore(*onTrueBlock);
  }

  size_t onFalseScore = 0;
  if (onFalseLegal) { //  && postDomTree.dominates(onTrueBlock, onFalseBlock)) {
    onFalseScore = getDomRegionScore(*onFalseBlock);
  }

  const size_t minScore = GetValue("BOSCC_LIMIT", 64);
  const size_t maxScore = 10000000;

// otw try to skip the bigger dominated part
  if (onTrueLegal &&
      onTrueScore <= maxScore && onTrueScore >= minScore &&
      onTrueScore > onFalseScore)
  {
    regScore = onTrueScore;
    return -1;
  }
  else if (onFalseLegal &&
      onFalseScore <= maxScore && onFalseScore >= minScore &&
      onFalseScore > onTrueScore)
  {
    regScore = onFalseScore;
    return 1;
  }

  // can not distinguish --> don't BOSCC
  // this holds e.g. if the branch does not dominate any of its successors
  return 0;
}

bool
run() {
  domTree.recalculate(vecInfo.getScalarFunction());

  size_t numBosccBranches = 0;

  ReversePostOrderTraversal<Function*> RPOT(&vecInfo.getScalarFunction());

  for (auto * BB : RPOT) {
    if (!vecInfo.inRegion(*BB)) continue;
    auto * term = BB->getTerminator();
    auto * branchInst = dyn_cast<BranchInst>(term);

  // consider divergent conditional branches
    if (!branchInst) continue;
    if (!branchInst->isConditional()) continue;
    if (vecInfo.getVectorShape(*branchInst).isUniform()) continue;

    size_t regScore = 0;
    int score = bosccHeuristic(*branchInst, regScore);
    if (score == 0) continue;
    int succIdx = score < 0 ? 0 : 1;

    ++numBosccBranches;

    Report() << "boscc: skip succ " << branchInst->getSuccessor(succIdx)->getName() << " of block " << branchInst->getParent()->getName() << "  score: " << regScore << "\n";
    transformBranch(*branchInst, succIdx);
  }

  if (numBosccBranches > 0) Report() << "boscc: inserted " << numBosccBranches << " BOSCC branches\n";

  // recover
  postDomTree.recalculate(vecInfo.getScalarFunction());

  domTree.verifyDomTree();

  return false;
}

};


bool
BOSCCTransform::run() {
  Impl impl(vecInfo, platInfo, domTree, postDomTree, loopInfo);
  return impl.run();
}


BOSCCTransform::BOSCCTransform(VectorizationInfo & _vecInfo, PlatformInfo & _platInfo, llvm::DominatorTree & _domTree, llvm::PostDominatorTree & _postDomTree, llvm::LoopInfo & _loopInfo)
: vecInfo(_vecInfo)
, platInfo(_platInfo)
, domTree(_domTree)
, postDomTree(_postDomTree)
, loopInfo(_loopInfo)
{}
