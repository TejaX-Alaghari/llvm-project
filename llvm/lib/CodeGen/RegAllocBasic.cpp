//===-- RegAllocBasic.cpp - Basic Register Allocator ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines the RABasic function pass, which provides a minimal
/// implementation of the basic register allocator.
///
//===----------------------------------------------------------------------===//

#include "RegAllocBasic.h"
#include "AllocationOrder.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocBasicPass.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "regalloc"

static RegisterRegAlloc basicRegAlloc("basic", "basic register allocator",
                                      createBasicRegisterAllocator);

//===----------------------------------------------------------------------===//
//                         RequiredAnalyses
//===----------------------------------------------------------------------===//

struct RABasic::RequiredAnalyses {
  VirtRegMap *VRM = nullptr;
  LiveIntervals *LIS = nullptr;
  LiveRegMatrix *LRM = nullptr;
  LiveStacks *LSS = nullptr;
  MachineBlockFrequencyInfo *MBFI = nullptr;
  MachineDominatorTree *DomTree = nullptr;
  MachineLoopInfo *Loops = nullptr;
  ProfileSummaryInfo *PSI = nullptr;

  RequiredAnalyses() = delete;
  RequiredAnalyses(Pass &P);
  RequiredAnalyses(MachineFunction &MF, MachineFunctionAnalysisManager &MFAM);
};

RABasic::RequiredAnalyses::RequiredAnalyses(Pass &P) {
  VRM = &P.getAnalysis<VirtRegMapWrapperLegacy>().getVRM();
  LIS = &P.getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  LRM = &P.getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM();
  LSS = &P.getAnalysis<LiveStacksWrapperLegacy>().getLS();
  MBFI = &P.getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  DomTree = &P.getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  Loops = &P.getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  PSI = &P.getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI();
}

RABasic::RequiredAnalyses::RequiredAnalyses(
    MachineFunction &MF, MachineFunctionAnalysisManager &MFAM) {
  VRM = &MFAM.getResult<VirtRegMapAnalysis>(MF);
  LIS = &MFAM.getResult<LiveIntervalsAnalysis>(MF);
  LRM = &MFAM.getResult<LiveRegMatrixAnalysis>(MF);
  LSS = &MFAM.getResult<LiveStacksAnalysis>(MF);
  MBFI = &MFAM.getResult<MachineBlockFrequencyAnalysis>(MF);
  DomTree = &MFAM.getResult<MachineDominatorTreeAnalysis>(MF);
  Loops = &MFAM.getResult<MachineLoopAnalysis>(MF);
  // PSI is optional for the basic allocator - it's only used for spill weight
  // calculation hints. In NPM, accessing module-level analyses is complex,
  // so we just pass nullptr.
  PSI = nullptr;
}

//===----------------------------------------------------------------------===//
//                         RABasic Implementation
//===----------------------------------------------------------------------===//

RABasic::RABasic(RequiredAnalyses &Analyses, const RegAllocFilterFunc F)
    : RegAllocBase(F) {
  VRM = Analyses.VRM;
  LIS = Analyses.LIS;
  Matrix = Analyses.LRM;
  LSS = Analyses.LSS;
  MBFI = Analyses.MBFI;
  DomTree = Analyses.DomTree;
  Loops = Analyses.Loops;
  PSI = Analyses.PSI;
}

//===----------------------------------------------------------------------===//
//                         RABasicLegacy (Legacy Pass)
//===----------------------------------------------------------------------===//

char RABasicLegacy::ID = 0;

char &llvm::RABasicID = RABasicLegacy::ID;

INITIALIZE_PASS_BEGIN(RABasicLegacy, "regallocbasic", "Basic Register Allocator",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariablesWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(RegisterCoalescerLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineSchedulerLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveStacksWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_END(RABasicLegacy, "regallocbasic", "Basic Register Allocator", false,
                    false)

RABasicLegacy::RABasicLegacy(const RegAllocFilterFunc F)
    : MachineFunctionPass(ID), F(F) {}

void RABasicLegacy::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addRequired<LiveIntervalsWrapperPass>();
  AU.addPreserved<LiveIntervalsWrapperPass>();
  AU.addPreserved<SlotIndexesWrapperPass>();
  AU.addRequired<LiveDebugVariablesWrapperLegacy>();
  AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
  AU.addRequired<LiveStacksWrapperLegacy>();
  AU.addPreserved<LiveStacksWrapperLegacy>();
  AU.addRequired<ProfileSummaryInfoWrapperPass>();
  AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
  AU.addPreserved<MachineBlockFrequencyInfoWrapperPass>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  AU.addRequiredID(MachineDominatorsID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addPreserved<MachineLoopInfoWrapperPass>();
  AU.addRequired<VirtRegMapWrapperLegacy>();
  AU.addPreserved<VirtRegMapWrapperLegacy>();
  AU.addRequired<LiveRegMatrixWrapperLegacy>();
  AU.addPreserved<LiveRegMatrixWrapperLegacy>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool RABasicLegacy::runOnMachineFunction(MachineFunction &MF) {
  RABasic::RequiredAnalyses Analyses(*this);
  RABasic Impl(Analyses, F);
  return Impl.run(MF);
}

//===----------------------------------------------------------------------===//
//                         RABasic Methods
//===----------------------------------------------------------------------===//

bool RABasic::LRE_CanEraseVirtReg(Register VirtReg) {
  LiveInterval &LI = LIS->getInterval(VirtReg);
  if (VRM->hasPhys(VirtReg)) {
    Matrix->unassign(LI);
    aboutToRemoveInterval(LI);
    return true;
  }
  // Unassigned virtreg is probably in the priority queue.
  // RegAllocBase will erase it after dequeueing.
  // Nonetheless, clear the live-range so that the debug
  // dump will show the right state for that VirtReg.
  LI.clear();
  return false;
}

void RABasic::LRE_WillShrinkVirtReg(Register VirtReg) {
  if (!VRM->hasPhys(VirtReg))
    return;

  // Register is assigned, put it back on the queue for reassignment.
  LiveInterval &LI = LIS->getInterval(VirtReg);
  Matrix->unassign(LI);
  enqueue(&LI);
}

void RABasic::releaseMemory() {
  SpillerInstance.reset();
}


// Spill or split all live virtual registers currently unified under PhysReg
// that interfere with VirtReg. The newly spilled or split live intervals are
// returned by appending them to SplitVRegs.
bool RABasic::spillInterferences(const LiveInterval &VirtReg,
                                 MCRegister PhysReg,
                                 SmallVectorImpl<Register> &SplitVRegs) {
  // Record each interference and determine if all are spillable before mutating
  // either the union or live intervals.
  SmallVector<const LiveInterval *, 8> Intfs;

  // Collect interferences assigned to any alias of the physical register.
  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    LiveIntervalUnion::Query &Q = Matrix->query(VirtReg, Unit);
    for (const auto *Intf : reverse(Q.interferingVRegs())) {
      if (!Intf->isSpillable() || Intf->weight() > VirtReg.weight())
        return false;
      Intfs.push_back(Intf);
    }
  }
  LLVM_DEBUG(dbgs() << "spilling " << printReg(PhysReg, TRI)
                    << " interferences with " << VirtReg << "\n");
  assert(!Intfs.empty() && "expected interference");

  // Spill each interfering vreg allocated to PhysReg or an alias.
  for (const LiveInterval *Spill : Intfs) {
    // Skip duplicates.
    if (!VRM->hasPhys(Spill->reg()))
      continue;

    // Deallocate the interfering vreg by removing it from the union.
    // A LiveInterval instance may not be in a union during modification!
    Matrix->unassign(*Spill);

    // Spill the extracted interval.
    LiveRangeEdit LRE(Spill, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
    spiller().spill(LRE);
  }
  return true;
}

// Driver for the register assignment and splitting heuristics.
// Manages iteration over the LiveIntervalUnions.
//
// This is a minimal implementation of register assignment and splitting that
// spills whenever we run out of registers.
//
// selectOrSplit can only be called once per live virtual register. We then do a
// single interference test for each register the correct class until we find an
// available register. So, the number of interference tests in the worst case is
// |vregs| * |machineregs|. And since the number of interference tests is
// minimal, there is no value in caching them outside the scope of
// selectOrSplit().
MCRegister RABasic::selectOrSplit(const LiveInterval &VirtReg,
                                  SmallVectorImpl<Register> &SplitVRegs) {
  // Populate a list of physical register spill candidates.
  SmallVector<MCRegister, 8> PhysRegSpillCands;

  // Check for an available register in this class.
  auto Order =
      AllocationOrder::create(VirtReg.reg(), *VRM, RegClassInfo, Matrix);
  for (MCRegister PhysReg : Order) {
    assert(PhysReg.isValid());
    // Check for interference in PhysReg
    switch (Matrix->checkInterference(VirtReg, PhysReg)) {
    case LiveRegMatrix::IK_Free:
      // PhysReg is available, allocate it.
      return PhysReg;

    case LiveRegMatrix::IK_VirtReg:
      // Only virtual registers in the way, we may be able to spill them.
      PhysRegSpillCands.push_back(PhysReg);
      continue;

    default:
      // RegMask or RegUnit interference.
      continue;
    }
  }

  // Try to spill another interfering reg with less spill weight.
  for (MCRegister &PhysReg : PhysRegSpillCands) {
    if (!spillInterferences(VirtReg, PhysReg, SplitVRegs))
      continue;

    assert(!Matrix->checkInterference(VirtReg, PhysReg) &&
           "Interference after spill.");
    // Tell the caller to allocate to this newly freed physical register.
    return PhysReg;
  }

  // No other spill candidates were found, so spill the current VirtReg.
  LLVM_DEBUG(dbgs() << "spilling: " << VirtReg << '\n');
  if (!VirtReg.isSpillable())
    return ~0u;
  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  spiller().spill(LRE);

  // The live virtual register requesting allocation was spilled, so tell
  // the caller not to allocate anything during this round.
  return 0;
}

bool RABasic::run(MachineFunction &mf) {
  LLVM_DEBUG(dbgs() << "********** BASIC REGISTER ALLOCATION **********\n"
                    << "********** Function: " << mf.getName() << '\n');

  MF = &mf;

  RegAllocBase::init(*VRM, *LIS, *Matrix);
  VirtRegAuxInfo VRAI(*MF, *LIS, *VRM, *Loops, *MBFI, PSI);
  VRAI.calculateSpillWeightsAndHints();

  SpillerInstance.reset(
      createInlineSpiller({*LIS, *LSS, *DomTree, *MBFI}, *MF, *VRM, VRAI));

  allocatePhysRegs();
  postOptimization();

  // Diagnostic output before rewriting
  LLVM_DEBUG(dbgs() << "Post alloc VirtRegMap:\n" << *VRM << "\n");

  releaseMemory();
  return true;
}

//===----------------------------------------------------------------------===//
//                         RABasicPass (New PM)
//===----------------------------------------------------------------------===//

PreservedAnalyses RABasicPass::run(MachineFunction &MF,
                                   MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);

  RABasic::RequiredAnalyses Analyses(MF, MFAM);
  RABasic Impl(Analyses, Opts.Filter);

  bool Changed = Impl.run(MF);
  if (!Changed)
    return PreservedAnalyses::all();

  auto PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<MachineBlockFrequencyAnalysis>();
  PA.preserve<LiveIntervalsAnalysis>();
  PA.preserve<SlotIndexesAnalysis>();
  PA.preserve<LiveDebugVariablesAnalysis>();
  PA.preserve<LiveStacksAnalysis>();
  PA.preserve<VirtRegMapAnalysis>();
  PA.preserve<LiveRegMatrixAnalysis>();
  return PA;
}

void RABasicPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) const {
  StringRef FilterName = Opts.FilterName.empty() ? "all" : Opts.FilterName;
  OS << "basic<" << FilterName << '>';
}

//===----------------------------------------------------------------------===//
//                         Factory Functions
//===----------------------------------------------------------------------===//

FunctionPass* llvm::createBasicRegisterAllocator() {
  return new RABasicLegacy();
}

FunctionPass *llvm::createBasicRegisterAllocator(RegAllocFilterFunc F) {
  return new RABasicLegacy(F);
}
