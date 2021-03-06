//== ---lib/CodeGen/GlobalISel/GICombinerHelper.cpp --------------------- == //
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "llvm/CodeGen/GlobalISel/Combiner.h"
#include "llvm/CodeGen/GlobalISel/CombinerHelper.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define DEBUG_TYPE "gi-combine"

using namespace llvm;

CombinerHelper::CombinerHelper(CombinerChangeObserver &Observer,
                               MachineIRBuilder &B)
    : Builder(B), MRI(Builder.getMF().getRegInfo()), Observer(Observer) {}

void CombinerHelper::eraseInstr(MachineInstr &MI) {
  Observer.erasedInstr(MI);
}
void CombinerHelper::scheduleForVisit(MachineInstr &MI) {
  Observer.createdInstr(MI);
}

bool CombinerHelper::tryCombineCopy(MachineInstr &MI) {
  if (MI.getOpcode() != TargetOpcode::COPY)
    return false;
  unsigned DstReg = MI.getOperand(0).getReg();
  unsigned SrcReg = MI.getOperand(1).getReg();
  LLT DstTy = MRI.getType(DstReg);
  LLT SrcTy = MRI.getType(SrcReg);
  // Simple Copy Propagation.
  // a(sx) = COPY b(sx) -> Replace all uses of a with b.
  if (DstTy.isValid() && SrcTy.isValid() && DstTy == SrcTy) {
    MI.eraseFromParent();
    MRI.replaceRegWith(DstReg, SrcReg);
    return true;
  }
  return false;
}

namespace {
struct PreferredTuple {
  LLT Ty;                // The result type of the extend.
  unsigned ExtendOpcode; // G_ANYEXT/G_SEXT/G_ZEXT
  MachineInstr *MI;
};

/// Select a preference between two uses. CurrentUse is the current preference
/// while *ForCandidate is attributes of the candidate under consideration.
PreferredTuple ChoosePreferredUse(PreferredTuple &CurrentUse,
                                  const LLT &TyForCandidate,
                                  unsigned OpcodeForCandidate,
                                  MachineInstr *MIForCandidate) {
  if (!CurrentUse.Ty.isValid()) {
    if (CurrentUse.ExtendOpcode == OpcodeForCandidate)
      return {TyForCandidate, OpcodeForCandidate, MIForCandidate};
    if (CurrentUse.ExtendOpcode == TargetOpcode::G_ANYEXT &&
        (OpcodeForCandidate == TargetOpcode::G_SEXT ||
         OpcodeForCandidate == TargetOpcode::G_ZEXT ||
         OpcodeForCandidate == TargetOpcode::G_ANYEXT))
      return {TyForCandidate, OpcodeForCandidate, MIForCandidate};
    return CurrentUse;
  }

  // We permit the extend to hoist through basic blocks but this is only
  // sensible if the target has extending loads. If you end up lowering back
  // into a load and extend during the legalizer then the end result is
  // hoisting the extend up to the load.

  // Prefer defined extensions to undefined extensions as these are more
  // likely to reduce the number of instructions.
  if (OpcodeForCandidate == TargetOpcode::G_ANYEXT &&
      CurrentUse.ExtendOpcode != TargetOpcode::G_ANYEXT)
    return CurrentUse;
  else if (CurrentUse.ExtendOpcode == TargetOpcode::G_ANYEXT &&
           OpcodeForCandidate != TargetOpcode::G_ANYEXT)
    return {TyForCandidate, OpcodeForCandidate, MIForCandidate};

  // Prefer sign extensions to zero extensions as sign-extensions tend to be
  // more expensive.
  if (CurrentUse.Ty == TyForCandidate) {
    if (CurrentUse.ExtendOpcode == TargetOpcode::G_SEXT &&
        OpcodeForCandidate == TargetOpcode::G_ZEXT)
      return CurrentUse;
    else if (CurrentUse.ExtendOpcode == TargetOpcode::G_ZEXT &&
             OpcodeForCandidate == TargetOpcode::G_SEXT)
      return {TyForCandidate, OpcodeForCandidate, MIForCandidate};
  }

  // This is potentially target specific. We've chosen the largest type
  // because G_TRUNC is usually free. One potential catch with this is that
  // some targets have a reduced number of larger registers than smaller
  // registers and this choice potentially increases the live-range for the
  // larger value.
  if (TyForCandidate.getSizeInBits() > CurrentUse.Ty.getSizeInBits()) {
    return {TyForCandidate, OpcodeForCandidate, MIForCandidate};
  }
  return CurrentUse;
};
} // end anonymous namespace

bool CombinerHelper::tryCombineExtendingLoads(MachineInstr &MI) {
  // We match the loads and follow the uses to the extend instead of matching
  // the extends and following the def to the load. This is because the load
  // must remain in the same position for correctness (unless we also add code
  // to find a safe place to sink it) whereas the extend is freely movable.
  // It also prevents us from duplicating the load for the volatile case or just
  // for performance.

  if (MI.getOpcode() != TargetOpcode::G_LOAD &&
      MI.getOpcode() != TargetOpcode::G_SEXTLOAD &&
      MI.getOpcode() != TargetOpcode::G_ZEXTLOAD)
    return false;

  auto &LoadValue = MI.getOperand(0);
  assert(LoadValue.isReg() && "Result wasn't a register?");

  LLT LoadValueTy = MRI.getType(LoadValue.getReg());
  if (!LoadValueTy.isScalar())
    return false;

  // Find the preferred type aside from the any-extends (unless it's the only
  // one) and non-extending ops. We'll emit an extending load to that type and
  // and emit a variant of (extend (trunc X)) for the others according to the
  // relative type sizes. At the same time, pick an extend to use based on the
  // extend involved in the chosen type.
  unsigned PreferredOpcode = MI.getOpcode() == TargetOpcode::G_LOAD
                                 ? TargetOpcode::G_ANYEXT
                                 : MI.getOpcode() == TargetOpcode::G_SEXTLOAD
                                       ? TargetOpcode::G_SEXT
                                       : TargetOpcode::G_ZEXT;
  PreferredTuple Preferred = {LLT(), PreferredOpcode, nullptr};
  for (auto &UseMI : MRI.use_instructions(LoadValue.getReg())) {
    if (UseMI.getOpcode() == TargetOpcode::G_SEXT ||
        UseMI.getOpcode() == TargetOpcode::G_ZEXT || !Preferred.Ty.isValid())
      Preferred = ChoosePreferredUse(Preferred,
                                     MRI.getType(UseMI.getOperand(0).getReg()),
                                     UseMI.getOpcode(), &UseMI);
  }

  // There were no extends
  if (!Preferred.MI)
    return false;
  // It should be impossible to chose an extend without selecting a different
  // type since by definition the result of an extend is larger.
  assert(Preferred.Ty != LoadValueTy && "Extending to same type?");

  // Rewrite the load and schedule the canonical use for erasure.
  const auto TruncateUse = [](MachineIRBuilder &Builder, MachineOperand &UseMO,
                              unsigned DstReg, unsigned SrcReg) {
    MachineInstr &UseMI = *UseMO.getParent();
    MachineBasicBlock &UseMBB = *UseMI.getParent();

    Builder.setInsertPt(UseMBB, MachineBasicBlock::iterator(UseMI));
    Builder.buildTrunc(DstReg, SrcReg);
  };

  // Rewrite the load to the chosen extending load.
  unsigned ChosenDstReg = Preferred.MI->getOperand(0).getReg();
  MI.setDesc(
      Builder.getTII().get(Preferred.ExtendOpcode == TargetOpcode::G_SEXT
                               ? TargetOpcode::G_SEXTLOAD
                               : Preferred.ExtendOpcode == TargetOpcode::G_ZEXT
                                     ? TargetOpcode::G_ZEXTLOAD
                                     : TargetOpcode::G_LOAD));

  // Rewrite all the uses to fix up the types.
  SmallVector<MachineInstr *, 1> ScheduleForErase;
  SmallVector<std::pair<MachineOperand*, unsigned>, 4> ScheduleForAssignReg;
  for (auto &UseMO : MRI.use_operands(LoadValue.getReg())) {
    MachineInstr *UseMI = UseMO.getParent();

    // If the extend is compatible with the preferred extend then we should fix
    // up the type and extend so that it uses the preferred use.
    if (UseMI->getOpcode() == Preferred.ExtendOpcode ||
        UseMI->getOpcode() == TargetOpcode::G_ANYEXT) {
      unsigned UseDstReg = UseMI->getOperand(0).getReg();
      unsigned UseSrcReg = UseMI->getOperand(1).getReg();
      const LLT &UseDstTy = MRI.getType(UseDstReg);
      if (UseDstReg != ChosenDstReg) {
        if (Preferred.Ty == UseDstTy) {
          // If the use has the same type as the preferred use, then merge
          // the vregs and erase the extend. For example:
          //    %1:_(s8) = G_LOAD ...
          //    %2:_(s32) = G_SEXT %1(s8)
          //    %3:_(s32) = G_ANYEXT %1(s8)
          //    ... = ... %3(s32)
          // rewrites to:
          //    %2:_(s32) = G_SEXTLOAD ...
          //    ... = ... %2(s32)
          MRI.replaceRegWith(UseDstReg, ChosenDstReg);
          ScheduleForErase.push_back(UseMO.getParent());
          Observer.erasedInstr(*UseMO.getParent());
        } else if (Preferred.Ty.getSizeInBits() < UseDstTy.getSizeInBits()) {
          // If the preferred size is smaller, then keep the extend but extend
          // from the result of the extending load. For example:
          //    %1:_(s8) = G_LOAD ...
          //    %2:_(s32) = G_SEXT %1(s8)
          //    %3:_(s64) = G_ANYEXT %1(s8)
          //    ... = ... %3(s64)
          /// rewrites to:
          //    %2:_(s32) = G_SEXTLOAD ...
          //    %3:_(s64) = G_ANYEXT %2:_(s32)
          //    ... = ... %3(s64)
          MRI.replaceRegWith(UseSrcReg, ChosenDstReg);
        } else {
          // If the preferred size is large, then insert a truncate. For
          // example:
          //    %1:_(s8) = G_LOAD ...
          //    %2:_(s64) = G_SEXT %1(s8)
          //    %3:_(s32) = G_ZEXT %1(s8)
          //    ... = ... %3(s32)
          /// rewrites to:
          //    %2:_(s64) = G_SEXTLOAD ...
          //    %4:_(s8) = G_TRUNC %2:_(s32)
          //    %3:_(s64) = G_ZEXT %2:_(s8)
          //    ... = ... %3(s64)
          unsigned NewVReg = MRI.cloneVirtualRegister(MI.getOperand(0).getReg());
          TruncateUse(Builder, UseMO, NewVReg, ChosenDstReg);
          ScheduleForAssignReg.emplace_back(&UseMO, NewVReg);
        }
        continue;
      }
      // The use is (one of) the uses of the preferred use we chose earlier.
      // We're going to update the load to def this value later so just erase
      // the old extend.
      ScheduleForErase.push_back(UseMO.getParent());
      Observer.erasedInstr(*UseMO.getParent());
      continue;
    }

    // The use isn't an extend. Truncate back to the type we originally loaded.
    // This is free on many targets.
    unsigned NewVReg = MRI.cloneVirtualRegister(MI.getOperand(0).getReg());
    TruncateUse(Builder, UseMO, NewVReg, ChosenDstReg);
    ScheduleForAssignReg.emplace_back(&UseMO, NewVReg);
  }
  for (auto &Assignment : ScheduleForAssignReg)
    Assignment.first->setReg(Assignment.second);
  for (auto &EraseMI : ScheduleForErase)
    EraseMI->eraseFromParent();
  MI.getOperand(0).setReg(ChosenDstReg);

  return true;
}

bool CombinerHelper::tryCombine(MachineInstr &MI) {
  if (tryCombineCopy(MI))
    return true;
  return tryCombineExtendingLoads(MI);
}
