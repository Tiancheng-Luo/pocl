// LLVM function pass add required barriers to loops.
// 
// Copyright (c) 2011 Universidad Rey Juan Carlos
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "LoopBarriers.h"
#include "Barrier.h"
#include "Workgroup.h"
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <iostream>

//#define DEBUG_LOOP_BARRIERS

using namespace llvm;
using namespace pocl;

namespace {
  static
  RegisterPass<LoopBarriers> X("loop-barriers",
                               "Add needed barriers to loops");
}

char LoopBarriers::ID = 0;

void
LoopBarriers::getAnalysisUsage(AnalysisUsage &AU) const
{
  AU.addRequired<DominatorTree>();
  AU.addPreserved<DominatorTree>();
}

bool
LoopBarriers::runOnLoop(Loop *L, LPPassManager &LPM)
{
  if (!Workgroup::isKernelToProcess(*L->getHeader()->getParent()))
    return false;

  DT = &getAnalysis<DominatorTree>();

  bool changed = ProcessLoop(L, LPM);

  DT->verifyAnalysis();

  return changed;
}


bool
LoopBarriers::ProcessLoop(Loop *L, LPPassManager &LPM)
{
  for (Loop::block_iterator i = L->block_begin(), e = L->block_end();
       i != e; ++i) {
    for (BasicBlock::iterator j = (*i)->begin(), e = (*i)->end();
         j != e; ++j) {
      if (isa<Barrier>(j)) {
        // Found a barrier on this loop, proceed:
        // 1) add a barrier on the loop header.
        // 2) add a barrier on the latches
        
        // Add a barrier on the preheader to ensure all WIs reach
        // the loop header with all the previous code already 
        // executed.
        BasicBlock *preheader = L->getLoopPreheader();
        assert((preheader != NULL) && "Non-canonicalized loop found!\n");
#ifdef DEBUG_LOOP_BARRIERS
        std::cerr << "### adding to preheader BB" << std::endl;
        preheader->dump();
        std::cerr << "### before instr" << std::endl;
        preheader->getTerminator()->dump();
#endif
        Barrier::Create(preheader->getTerminator());
        preheader->setName(preheader->getName() + ".loopbarrier");

#if 0
        /* In case the loop is conditional, that is, it
           can be skipped completely, add a barrier to the
           branch block so it won't get replicated multiple
           times. This situation happens when one has 
           a compile-time unknown variable iteration count which
           can be zero, or if the iteration variable is volatile
           in which case LLVM inserts a loop skip condition
           just after initializing the loop variable.

           FIXME: this is dangerous as the loop condition might
           be using WI IDs. Cannot do this unconditionally.
 */
        BasicBlock *condBB = preheader->getSinglePredecessor();
        if (condBB != NULL && condBB->getTerminator() != NULL &&
            condBB->getTerminator()->getNumSuccessors() > 1)
          {
#ifdef DEBUG_LOOP_BARRIERS
            std::cerr << "### loop skip BB: " << std::endl;
            condBB->dump();
#endif
            Barrier::Create(condBB->getTerminator());
            condBB->setName(condBB->getName() + ".loopskipbarrier");
          }
#endif

        // Add a barrier after the PHI nodes on the header (the replicated
        // headers will be merged afterwards).
        BasicBlock *header = L->getHeader();
        if (header->getFirstNonPHI() != &header->front()) {
          Barrier::Create(header->getFirstNonPHI());
          header->setName(header->getName() + ".phibarrier");
        }

        // Add the barriers on the exiting block and the latches,
        // which might not always be the same if there is computation
        // after the exit decision.
        BasicBlock *brexit = L->getExitingBlock();
        if (brexit != NULL) {
          Barrier::Create(brexit->getTerminator());
          brexit->setName(brexit->getName() + ".brexitbarrier");
        }

        BasicBlock *latch = L->getLoopLatch();
        if (latch != NULL && brexit != latch) {
          // This loop has only one latch. Do not check for dominance, we
          // are probably running before BTR.
          Barrier::Create(latch->getTerminator());
          latch->setName(latch->getName() + ".latchbarrier");
          return true;
        }

        // Modified code from llvm::LoopBase::getLoopLatch to
        // go trough all the latches.
        BasicBlock *Header = L->getHeader();
        typedef GraphTraits<Inverse<BasicBlock *> > InvBlockTraits;
        InvBlockTraits::ChildIteratorType PI = InvBlockTraits::child_begin(Header);
        InvBlockTraits::ChildIteratorType PE = InvBlockTraits::child_end(Header);
        BasicBlock *Latch = NULL;
        for (; PI != PE; ++PI) {
          InvBlockTraits::NodeType *N = *PI;
          if (L->contains(N)) {
            Latch = N;
            // Latch found in the loop, see if the barrier dominates it
            // (otherwise if might no even belong to this "tail", see
            // forifbarrier1 graph test).
            if (DT->dominates(j->getParent(), Latch)) {
              Barrier::Create(Latch->getTerminator());
              Latch->setName(Latch->getName() + ".latchbarrier");
            }
          }
        }

        return true;
      }
    }
  }

  /* This is a loop without a barrier. Ensure we have a non-barrier
     block as a preheader so we can replicate the loop as a whole. 

     If the block has proper instructions after the barrier, it
     will be split in CanonicalizeBarriers. */
  BasicBlock *preheader = L->getLoopPreheader();
  assert((preheader != NULL) && "Non-canonicalized loop found!\n");
  TerminatorInst *t = preheader->getTerminator();
  Instruction *prev = NULL;
  if (&preheader->front() != t)
    prev = t->getPrevNode();
  if (prev && isa<Barrier>(prev))
    {
      BasicBlock *new_b = SplitBlock(preheader, t, this);
      new_b->setName(preheader->getName() + ".postbarrier_dummy");
      return true;
    }

  return false;
}
