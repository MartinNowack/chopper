//===-- Searcher.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Searcher.h"

#include "CoreStats.h"
#include "Executor.h"
#include "PTree.h"
#include "StatsTracker.h"

#include "klee/ExecutionState.h"
#include "klee/Statistics.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/ADT/DiscretePDF.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Support/ModuleUtil.h"
#include "klee/Internal/System/Time.h"
#include "klee/Internal/Support/ErrorHandling.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#else
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#endif
#include "llvm/Support/CommandLine.h"

#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
#include "llvm/Support/CallSite.h"
#else
#include "llvm/IR/CallSite.h"
#endif

#include <cassert>
#include <fstream>
#include <climits>

using namespace klee;
using namespace llvm;

namespace {
  cl::opt<bool>
  DebugLogMerge("debug-log-merge");
}

namespace klee {
  extern RNG theRNG;
}

Searcher::~Searcher() {
}

///

ExecutionState &DFSSearcher::selectState() {
  return *states.back();
}

void DFSSearcher::update(ExecutionState *current,
                         const std::vector<ExecutionState *> &addedStates,
                         const std::vector<ExecutionState *> &removedStates) {
  states.insert(states.end(),
                addedStates.begin(),
                addedStates.end());
  for (std::vector<ExecutionState *>::const_iterator it = removedStates.begin(),
                                                     ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    if (es == states.back()) {
      states.pop_back();
    } else {
      bool ok = false;

      for (std::vector<ExecutionState*>::iterator it = states.begin(),
             ie = states.end(); it != ie; ++it) {
        if (es==*it) {
          states.erase(it);
          ok = true;
          break;
        }
      }

      assert(ok && "invalid state removed");
    }
  }
}

///

ExecutionState &BFSSearcher::selectState() {
  return *states.front();
}

void BFSSearcher::update(ExecutionState *current,
                         const std::vector<ExecutionState *> &addedStates,
                         const std::vector<ExecutionState *> &removedStates) {
  // Assumption: If new states were added KLEE forked, therefore states evolved.
  // constraints were added to the current state, it evolved.
  if (!addedStates.empty() && current &&
      std::find(removedStates.begin(), removedStates.end(), current) ==
          removedStates.end()) {
    if (states.front() == current) {
      // BFS is the only searcher
      states.pop_front();
    } else {
      // interleaved with other searcher
      std::deque<ExecutionState*>::iterator pos = std::find(states.begin(),
                                                            states.end(),
                                                            current);
      assert(pos != states.end());
      states.erase(pos);
    }
    states.push_back(current);
  }

  states.insert(states.end(),
                addedStates.begin(),
                addedStates.end());
  for (std::vector<ExecutionState *>::const_iterator it = removedStates.begin(),
                                                     ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    if (es == states.front()) {
      states.pop_front();
    } else {
      bool ok = false;

      for (std::deque<ExecutionState*>::iterator it = states.begin(),
             ie = states.end(); it != ie; ++it) {
        if (es==*it) {
          states.erase(it);
          ok = true;
          break;
        }
      }

      assert(ok && "invalid state removed");
    }
  }
}

///

ExecutionState &RandomSearcher::selectState() {
  return *states[theRNG.getInt32()%states.size()];
}

void
RandomSearcher::update(ExecutionState *current,
                       const std::vector<ExecutionState *> &addedStates,
                       const std::vector<ExecutionState *> &removedStates) {
  states.insert(states.end(),
                addedStates.begin(),
                addedStates.end());
  for (std::vector<ExecutionState *>::const_iterator it = removedStates.begin(),
                                                     ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    bool ok = false;

    for (std::vector<ExecutionState*>::iterator it = states.begin(),
           ie = states.end(); it != ie; ++it) {
      if (es==*it) {
        states.erase(it);
        ok = true;
        break;
      }
    }
    
    assert(ok && "invalid state removed");
  }
}

///

WeightedRandomSearcher::WeightedRandomSearcher(WeightType _type)
  : states(new DiscretePDF<ExecutionState*>()),
    type(_type) {
  switch(type) {
  case Depth: 
    updateWeights = false;
    break;
  case InstCount:
  case CPInstCount:
  case QueryCost:
  case MinDistToUncovered:
  case CoveringNew:
  case PatchTesting:
    updateWeights = true;
    break;
  default:
    assert(0 && "invalid weight type");
  }
}

WeightedRandomSearcher::~WeightedRandomSearcher() {
  delete states;
}

ExecutionState &WeightedRandomSearcher::selectState() {
  return *states->choose(theRNG.getDoubleL());
}

double WeightedRandomSearcher::getWeight(ExecutionState *es) {
  switch(type) {
  default:
  case Depth: 
    return es->weight;
  case InstCount: {
    uint64_t count = theStatisticManager->getIndexedValue(stats::instructions,
                                                          es->pc->info->id);
    double inv = 1. / std::max((uint64_t) 1, count);
    return inv * inv;
  }
  case CPInstCount: {
    StackFrame &sf = es->stack.back();
    uint64_t count = sf.callPathNode->statistics.getValue(stats::instructions);
    double inv = 1. / std::max((uint64_t) 1, count);
    return inv;
  }
  case QueryCost:
    return (es->queryCost < .1) ? 1. : 1./es->queryCost;
  case CoveringNew:
  case MinDistToUncovered: {
    uint64_t md2u = computeMinDistToUncovered(es->pc,
                                              es->stack.back().minDistToUncoveredOnReturn);

    double invMD2U = 1. / (md2u ? md2u : 10000);
    if (type == CoveringNew || type == PatchTesting) {
      double invCovNew = 0.;
      if (es->instsSinceCovNew)
        invCovNew = 1. / std::max(1, (int) es->instsSinceCovNew - 1000);
      return (invCovNew * invCovNew + invMD2U * invMD2U);
    } else {
      return invMD2U * invMD2U;
    }
  }
  case PatchTesting: {
    uint64_t md2u = computeMinDistToCall(
        es->pc, es->stack.back().minDistToUncoveredOnReturn);

    double invMD2U = 1. / (md2u ? md2u : 10000);
    double invCovNew = 0.;
    if (es->instsSinceCovNew)
      invCovNew = 1. / std::max(1, (int)es->instsSinceCovNew - 1000);
    return (invCovNew * invCovNew + invMD2U * invMD2U);
  }
  }
}

void WeightedRandomSearcher::update(
    ExecutionState *current, const std::vector<ExecutionState *> &addedStates,
    const std::vector<ExecutionState *> &removedStates) {
  if (current && updateWeights &&
      std::find(removedStates.begin(), removedStates.end(), current) ==
          removedStates.end())
    states->update(current, getWeight(current));

  for (std::vector<ExecutionState *>::const_iterator it = addedStates.begin(),
                                                     ie = addedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    states->insert(es, getWeight(es));
  }

  for (std::vector<ExecutionState *>::const_iterator it = removedStates.begin(),
                                                     ie = removedStates.end();
       it != ie; ++it) {
    states->remove(*it);
  }
}

bool WeightedRandomSearcher::empty() { 
  return states->empty(); 
}

///

RandomPathSearcher::RandomPathSearcher(Executor &_executor)
  : executor(_executor) {
}

RandomPathSearcher::~RandomPathSearcher() {
}

ExecutionState &RandomPathSearcher::selectState() {
  unsigned flips=0, bits=0;
  PTree::Node *n = executor.processTree->root;
  
  while (!n->data) {
    if (!n->left) {
      n = n->right;
    } else if (!n->right) {
      n = n->left;
    } else {
      if (bits==0) {
        flips = theRNG.getInt32();
        bits = 32;
      }
      --bits;
      n = (flips&(1<<bits)) ? n->left : n->right;
    }
  }

  ExecutionState *es = n->data;
  while (es->isSuspended()) {
    es = es->getRecoveryState();
  }
  return *es;
}

void
RandomPathSearcher::update(ExecutionState *current,
                           const std::vector<ExecutionState *> &addedStates,
                           const std::vector<ExecutionState *> &removedStates) {
}

bool RandomPathSearcher::empty() { 
  return executor.states.empty(); 
}

///

BumpMergingSearcher::BumpMergingSearcher(Executor &_executor, Searcher *_baseSearcher) 
  : executor(_executor),
    baseSearcher(_baseSearcher),
    mergeFunction(executor.kmodule->kleeMergeFn) {
}

BumpMergingSearcher::~BumpMergingSearcher() {
  delete baseSearcher;
}

///

Instruction *BumpMergingSearcher::getMergePoint(ExecutionState &es) {  
  if (mergeFunction) {
    Instruction *i = es.pc->inst;

    if (i->getOpcode()==Instruction::Call) {
      CallSite cs(cast<CallInst>(i));
      if (mergeFunction==cs.getCalledFunction())
        return i;
    }
  }

  return 0;
}

ExecutionState &BumpMergingSearcher::selectState() {
entry:
  // out of base states, pick one to pop
  if (baseSearcher->empty()) {
    std::map<llvm::Instruction*, ExecutionState*>::iterator it = 
      statesAtMerge.begin();
    ExecutionState *es = it->second;
    statesAtMerge.erase(it);
    ++es->pc;

    baseSearcher->addState(es);
  }

  ExecutionState &es = baseSearcher->selectState();

  if (Instruction *mp = getMergePoint(es)) {
    std::map<llvm::Instruction*, ExecutionState*>::iterator it = 
      statesAtMerge.find(mp);

    baseSearcher->removeState(&es);

    if (it==statesAtMerge.end()) {
      statesAtMerge.insert(std::make_pair(mp, &es));
    } else {
      ExecutionState *mergeWith = it->second;
      if (mergeWith->merge(es)) {
        // hack, because we are terminating the state we need to let
        // the baseSearcher know about it again
        baseSearcher->addState(&es);
        executor.terminateState(es);
      } else {
        it->second = &es; // the bump
        ++mergeWith->pc;

        baseSearcher->addState(mergeWith);
      }
    }

    goto entry;
  } else {
    return es;
  }
}

void BumpMergingSearcher::update(
    ExecutionState *current, const std::vector<ExecutionState *> &addedStates,
    const std::vector<ExecutionState *> &removedStates) {
  baseSearcher->update(current, addedStates, removedStates);
}

///

MergingSearcher::MergingSearcher(Executor &_executor, Searcher *_baseSearcher) 
  : executor(_executor),
    baseSearcher(_baseSearcher),
    mergeFunction(executor.kmodule->kleeMergeFn) {
}

MergingSearcher::~MergingSearcher() {
  delete baseSearcher;
}

///

Instruction *MergingSearcher::getMergePoint(ExecutionState &es) {
  if (mergeFunction) {
    Instruction *i = es.pc->inst;

    if (i->getOpcode()==Instruction::Call) {
      CallSite cs(cast<CallInst>(i));
      if (mergeFunction==cs.getCalledFunction())
        return i;
    }
  }

  return 0;
}

ExecutionState &MergingSearcher::selectState() {
  // FIXME: this loop is endless if baseSearcher includes RandomPathSearcher.
  // The reason is that RandomPathSearcher::removeState() does nothing...
  while (!baseSearcher->empty()) {
    ExecutionState &es = baseSearcher->selectState();
    if (getMergePoint(es)) {
      baseSearcher->removeState(&es, &es);
      statesAtMerge.insert(&es);
    } else {
      return es;
    }
  }
  
  // build map of merge point -> state list
  std::map<Instruction*, std::vector<ExecutionState*> > merges;
  for (std::set<ExecutionState*>::const_iterator it = statesAtMerge.begin(),
         ie = statesAtMerge.end(); it != ie; ++it) {
    ExecutionState &state = **it;
    Instruction *mp = getMergePoint(state);
    
    merges[mp].push_back(&state);
  }
  
  if (DebugLogMerge)
    llvm::errs() << "-- all at merge --\n";
  for (std::map<Instruction*, std::vector<ExecutionState*> >::iterator
         it = merges.begin(), ie = merges.end(); it != ie; ++it) {
    if (DebugLogMerge) {
      llvm::errs() << "\tmerge: " << it->first << " [";
      for (std::vector<ExecutionState*>::iterator it2 = it->second.begin(),
             ie2 = it->second.end(); it2 != ie2; ++it2) {
        ExecutionState *state = *it2;
        llvm::errs() << state << ", ";
      }
      llvm::errs() << "]\n";
    }

    // merge states
    std::set<ExecutionState*> toMerge(it->second.begin(), it->second.end());
    while (!toMerge.empty()) {
      ExecutionState *base = *toMerge.begin();
      toMerge.erase(toMerge.begin());
      
      std::set<ExecutionState*> toErase;
      for (std::set<ExecutionState*>::iterator it = toMerge.begin(),
             ie = toMerge.end(); it != ie; ++it) {
        ExecutionState *mergeWith = *it;
        
        if (base->merge(*mergeWith)) {
          toErase.insert(mergeWith);
        }
      }
      if (DebugLogMerge && !toErase.empty()) {
        llvm::errs() << "\t\tmerged: " << base << " with [";
        for (std::set<ExecutionState*>::iterator it = toErase.begin(),
               ie = toErase.end(); it != ie; ++it) {
          if (it!=toErase.begin()) llvm::errs() << ", ";
          llvm::errs() << *it;
        }
        llvm::errs() << "]\n";
      }
      for (std::set<ExecutionState*>::iterator it = toErase.begin(),
             ie = toErase.end(); it != ie; ++it) {
        std::set<ExecutionState*>::iterator it2 = toMerge.find(*it);
        assert(it2!=toMerge.end());
        executor.terminateState(**it);
        toMerge.erase(it2);
      }

      // step past merge and toss base back in pool
      statesAtMerge.erase(statesAtMerge.find(base));
      ++base->pc;
      baseSearcher->addState(base);
    }  
  }
  
  if (DebugLogMerge)
    llvm::errs() << "-- merge complete, continuing --\n";
  
  return selectState();
}

void
MergingSearcher::update(ExecutionState *current,
                        const std::vector<ExecutionState *> &addedStates,
                        const std::vector<ExecutionState *> &removedStates) {
  if (!removedStates.empty()) {
    std::vector<ExecutionState *> alt = removedStates;
    for (std::vector<ExecutionState *>::const_iterator
             it = removedStates.begin(),
             ie = removedStates.end();
         it != ie; ++it) {
      ExecutionState *es = *it;
      std::set<ExecutionState*>::const_iterator it2 = statesAtMerge.find(es);
      if (it2 != statesAtMerge.end()) {
        statesAtMerge.erase(it2);
        alt.erase(std::remove(alt.begin(), alt.end(), es), alt.end());
      }
    }    
    baseSearcher->update(current, addedStates, alt);
  } else {
    baseSearcher->update(current, addedStates, removedStates);
  }
}

///

BatchingSearcher::BatchingSearcher(Searcher *_baseSearcher,
                                   double _timeBudget,
                                   unsigned _instructionBudget) 
  : baseSearcher(_baseSearcher),
    timeBudget(_timeBudget),
    instructionBudget(_instructionBudget),
    lastState(0) {
  
}

BatchingSearcher::~BatchingSearcher() {
  delete baseSearcher;
}

ExecutionState &BatchingSearcher::selectState() {
  if (!lastState || 
      (util::getWallTime()-lastStartTime)>timeBudget ||
      (stats::instructions-lastStartInstructions)>instructionBudget) {
    if (lastState) {
      double delta = util::getWallTime()-lastStartTime;
      if (delta>timeBudget*1.1) {
        klee_message("KLEE: increased time budget from %f to %f\n", timeBudget,
                     delta);
        timeBudget = delta;
      }
    }
    lastState = &baseSearcher->selectState();
    lastStartTime = util::getWallTime();
    lastStartInstructions = stats::instructions;
    return *lastState;
  } else {
    return *lastState;
  }
}

void
BatchingSearcher::update(ExecutionState *current,
                         const std::vector<ExecutionState *> &addedStates,
                         const std::vector<ExecutionState *> &removedStates) {
  if (std::find(removedStates.begin(), removedStates.end(), lastState) !=
      removedStates.end())
    lastState = 0;
  baseSearcher->update(current, addedStates, removedStates);
}

/***/

IterativeDeepeningTimeSearcher::IterativeDeepeningTimeSearcher(Searcher *_baseSearcher)
  : baseSearcher(_baseSearcher),
    time(1.) {
}

IterativeDeepeningTimeSearcher::~IterativeDeepeningTimeSearcher() {
  delete baseSearcher;
}

ExecutionState &IterativeDeepeningTimeSearcher::selectState() {
  ExecutionState &res = baseSearcher->selectState();
  startTime = util::getWallTime();
  return res;
}

void IterativeDeepeningTimeSearcher::update(
    ExecutionState *current, const std::vector<ExecutionState *> &addedStates,
    const std::vector<ExecutionState *> &removedStates) {
  double elapsed = util::getWallTime() - startTime;

  if (!removedStates.empty()) {
    std::vector<ExecutionState *> alt = removedStates;
    for (std::vector<ExecutionState *>::const_iterator
             it = removedStates.begin(),
             ie = removedStates.end();
         it != ie; ++it) {
      ExecutionState *es = *it;
      std::set<ExecutionState*>::const_iterator it2 = pausedStates.find(es);
      if (it2 != pausedStates.end()) {
        pausedStates.erase(it2);
        alt.erase(std::remove(alt.begin(), alt.end(), es), alt.end());
      }
    }    
    baseSearcher->update(current, addedStates, alt);
  } else {
    baseSearcher->update(current, addedStates, removedStates);
  }

  if (current &&
      std::find(removedStates.begin(), removedStates.end(), current) ==
          removedStates.end() &&
      elapsed > time) {
    pausedStates.insert(current);
    baseSearcher->removeState(current);
  }

  if (baseSearcher->empty()) {
    time *= 2;
    klee_message("KLEE: increased time budget to %f\n", time);
    std::vector<ExecutionState *> ps(pausedStates.begin(), pausedStates.end());
    baseSearcher->update(0, ps, std::vector<ExecutionState *>());
    pausedStates.clear();
  }
}

/***/

InterleavedSearcher::InterleavedSearcher(const std::vector<Searcher*> &_searchers)
  : searchers(_searchers),
    index(1) {
}

InterleavedSearcher::~InterleavedSearcher() {
  for (std::vector<Searcher*>::const_iterator it = searchers.begin(),
         ie = searchers.end(); it != ie; ++it)
    delete *it;
}

ExecutionState &InterleavedSearcher::selectState() {
  Searcher *s = searchers[--index];
  if (index==0) index = searchers.size();
  return s->selectState();
}

void InterleavedSearcher::update(
    ExecutionState *current, const std::vector<ExecutionState *> &addedStates,
    const std::vector<ExecutionState *> &removedStates) {
  for (std::vector<Searcher*>::const_iterator it = searchers.begin(),
         ie = searchers.end(); it != ie; ++it)
    (*it)->update(current, addedStates, removedStates);
}

/* splitted searcher */
SplittedSearcher::SplittedSearcher(Searcher *baseSearcher, Searcher *recoverySearcher, unsigned int ratio)
  : baseSearcher(baseSearcher), recoverySearcher(recoverySearcher), ratio(ratio)
{

}

SplittedSearcher::~SplittedSearcher() {
  delete baseSearcher;
}

ExecutionState &SplittedSearcher::selectState() {
  if (baseSearcher->empty()) {
    /* the recovery states are supposed to be not empty */
    return recoverySearcher->selectState();
  }

  if (recoverySearcher->empty()) {
    /* the base searcher is supposed to be not empty */
    return baseSearcher->selectState();
  }

  /* in this case, both searchers are supposed to be not empty */
  if (theRNG.getInt32() % 100 < ratio) {
    /* we handle recovery states in a DFS manner */
    return recoverySearcher->selectState();
  } else {
    return baseSearcher->selectState();
  }
}

void SplittedSearcher::update(
  ExecutionState *current,
  const std::vector<ExecutionState *> &addedStates,
  const std::vector<ExecutionState *> &removedStates
) {
  std::vector<ExecutionState *> addedOriginatingStates;
  std::vector<ExecutionState *> addedRecoveryStates;
  std::vector<ExecutionState *> removedOriginatingStates;
  std::vector<ExecutionState *> removedRecoveryStates;

  /* split added states */
  for (auto i = addedStates.begin(); i != addedStates.end(); i++) {
    ExecutionState *es = *i;
    if (es->isRecoveryState()) {
      addedRecoveryStates.push_back(es);
    } else {
      addedOriginatingStates.push_back(es);
    }
  }

  /* split removed states */
  for (auto i = removedStates.begin(); i != removedStates.end(); i++) {
    ExecutionState *es = *i;
    if (es->isRecoveryState()) {
      removedRecoveryStates.push_back(es);
    } else {
      removedOriginatingStates.push_back(es);
    }
  }

  if (current && current->isRecoveryState()) {
    baseSearcher->update(NULL, addedOriginatingStates, removedOriginatingStates);
  } else {
    baseSearcher->update(current, addedOriginatingStates, removedOriginatingStates);
  }

  if (current && !current->isRecoveryState()) {
    recoverySearcher->update(NULL, addedRecoveryStates, removedRecoveryStates);
  } else {
    recoverySearcher->update(current, addedRecoveryStates, removedRecoveryStates);
  }
}

bool SplittedSearcher::empty() {
  return baseSearcher->empty() && recoverySearcher->empty();
}

/* random-path searcher for the recovery model */
RandomRecoveryPath::RandomRecoveryPath(Executor &executor)
  : executor(executor)
{

}

RandomRecoveryPath::~RandomRecoveryPath() {

}

ExecutionState &RandomRecoveryPath::selectState() {
  if (treeStack.empty()) {
    /* as this point, the order of selection does not matter */
    return *states.front();
  }

  unsigned int flips = 0;
  unsigned int bits = 0;

  /* select the root */
  PTree::Node *n = treeStack.top();

  while (!n->data) {
    if (!n->left) {
      n = n->right;
    } else if (!n->right) {
      n = n->left;
    } else {
      if (bits==0) {
        flips = theRNG.getInt32();
        bits = 32;
      }
      --bits;
      n = (flips&(1<<bits)) ? n->left : n->right;
    }
  }

  ExecutionState *es = n->data;
  while (es->isSuspended()) {
    es = es->getRecoveryState();
  }
  return *es;
}

void RandomRecoveryPath::update(
  ExecutionState *current,
  const std::vector<ExecutionState *> &addedStates,
  const std::vector<ExecutionState *> &removedStates
) {
  for (auto i = addedStates.begin(); i != addedStates.end(); i++) {
    ExecutionState *es = *i;
    if (es->getLevel() == treeStack.size()) {
      /* this state has a higher level, so we push it as a root */
      treeStack.push(es->ptreeNode);
    }

    /* add state */
    states.push_back(es);
  }
  for (auto i = removedStates.begin(); i != removedStates.end(); i++) {
    ExecutionState *es = *i;
    /* a top level recovery state terminated, so we pop it's root from the stack */
    if (es->isResumed() && es->getLevel() == treeStack.size() - 1) {
      treeStack.pop();
    }

    /* remove state */
    for (auto j = states.begin(); j != states.end(); j++) {
      if (es == *j) {
        states.erase(j);
        break;
      }
    }
  }
}

bool RandomRecoveryPath::empty() {
  return treeStack.empty() && states.empty();
}

/* optimized splitted searcher */
OptimizedSplittedSearcher::OptimizedSplittedSearcher(
  Searcher *baseSearcher,
  Searcher *recoverySearcher,
  Searcher *highPrioritySearcher,
  unsigned int ratio
) :
  baseSearcher(baseSearcher),
  recoverySearcher(recoverySearcher),
  highPrioritySearcher(highPrioritySearcher),
  ratio(ratio)
{

}

OptimizedSplittedSearcher::~OptimizedSplittedSearcher() {
  delete highPrioritySearcher;
  delete recoverySearcher;
  delete baseSearcher;
}

ExecutionState &OptimizedSplittedSearcher::selectState() {
  /* high priority recovery states must be considered first */
  if (!highPrioritySearcher->empty()) {
    return highPrioritySearcher->selectState();
  }

  if (baseSearcher->empty()) {
    /* the recovery states are supposed to be not empty */
    return recoverySearcher->selectState();
  }

  if (recoverySearcher->empty()) {
    /* the base searcher is supposed to be not empty */
    return baseSearcher->selectState();
  }

  /* in this case, both searchers are supposed to be not empty */
  if (theRNG.getInt32() % 100 < ratio) {
    /* we handle recovery states in a DFS manner */
    return recoverySearcher->selectState();
  } else {
    return baseSearcher->selectState();
  }
}

void OptimizedSplittedSearcher::update(
  ExecutionState *current,
  const std::vector<ExecutionState *> &addedStates,
  const std::vector<ExecutionState *> &removedStates
) {
  std::vector<ExecutionState *> addedOriginatingStates;
  std::vector<ExecutionState *> addedRecoveryStates;
  std::vector<ExecutionState *> removedOriginatingStates;
  std::vector<ExecutionState *> removedRecoveryStates;

  /* split added states */
  for (auto i = addedStates.begin(); i != addedStates.end(); i++) {
    ExecutionState *es = *i;
    if (es->isRecoveryState()) {
      if (es->getPriority() == PRIORITY_HIGH) {
        highPrioritySearcher->addState(es);
      } else {
        addedRecoveryStates.push_back(es);
      }
    } else {
      addedOriginatingStates.push_back(es);
    }
  }

  /* split removed states */
  for (auto i = removedStates.begin(); i != removedStates.end(); i++) {
    ExecutionState *es = *i;
    if (es->isRecoveryState()) {
      if (es->getPriority() == PRIORITY_HIGH) {
        highPrioritySearcher->removeState(es);
        /* flush the high priority recovery states, only when a root recovery state terminates */
        if ((es->isResumed() && es->getLevel() == 0)) {
          int count = 0;
          while (!highPrioritySearcher->empty()) {
            ExecutionState &rs = highPrioritySearcher->selectState();
            highPrioritySearcher->removeState(&rs);
            rs.setPriority(PRIORITY_LOW);
            recoverySearcher->addState(&rs);
            count++;
          }
        }
      } else {
        removedRecoveryStates.push_back(es);
      }
    } else {
      removedOriginatingStates.push_back(es);
    }
  }

  if (current && current->isRecoveryState()) {
    baseSearcher->update(NULL, addedOriginatingStates, removedOriginatingStates);
  } else {
    baseSearcher->update(current, addedOriginatingStates, removedOriginatingStates);
  }

  if (current && !current->isRecoveryState()) {
    recoverySearcher->update(NULL, addedRecoveryStates, removedRecoveryStates);
  } else {
    recoverySearcher->update(current, addedRecoveryStates, removedRecoveryStates);
  }
}

bool OptimizedSplittedSearcher::empty() {
  return baseSearcher->empty() && recoverySearcher->empty() && highPrioritySearcher->empty();
}
