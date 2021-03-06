//===-- EpiphanyAsmPrinter.cpp - Print machine code to an Epiphany .s file --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to GAS-format Epiphany assembly language.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "asm-printer"
#include "EpiphanyAsmPrinter.h"
#include "InstPrinter/EpiphanyInstPrinter.h"
#include "llvm/DebugInfo.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/CodeGen/MachineModuleInfoImpls.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/Mangler.h"

using namespace llvm;

MachineLocation
EpiphanyAsmPrinter::getDebugValueLocation(const MachineInstr *MI) const {
  // See emitFrameIndexDebugValue in InstrInfo for where this instruction is
  // expected to be created.
  assert(MI->getNumOperands() == 4 && MI->getOperand(0).isReg()
         && MI->getOperand(1).isImm() && "unexpected custom DBG_VALUE");
  return MachineLocation(MI->getOperand(0).getReg(),
                         MI->getOperand(1).getImm());
}

/// Try to print a floating-point register as if it belonged to a specified
/// register-class. For example the inline asm operand modifier "b" requires its
/// argument to be printed as "bN".
static bool printModifiedFPRAsmOperand(const MachineOperand &MO,
                                       const TargetRegisterInfo *TRI,
                                       const TargetRegisterClass &RegClass,
                                       raw_ostream &O) {
  if (!MO.isReg())
    return true;

  for (MCRegAliasIterator AR(MO.getReg(), TRI, true); AR.isValid(); ++AR) {
    if (RegClass.contains(*AR)) {
      O << EpiphanyInstPrinter::getRegisterName(*AR);
      return false;
    }
  }
  return true;
}

/// Implements the 'w' and 'x' inline asm operand modifiers, which print a GPR
/// with the obvious type and an immediate 0 as either wzr or xzr.
static bool printModifiedGPRAsmOperand(const MachineOperand &MO,
                                       const TargetRegisterInfo *TRI,
                                       const TargetRegisterClass &RegClass,
                                       raw_ostream &O) {
  char Prefix = &RegClass == &Epiphany::GPR32RegClass ? 'w' : 'x';

  if (MO.isImm() && MO.getImm() == 0) {
    O << Prefix << "zr";
    return false;
  } else if (MO.isReg()) {
    if (MO.getReg() == Epiphany::SP) {
      O << (Prefix == 'x' ? "sp" : "wsp");
      return false;
    }

    for (MCRegAliasIterator AR(MO.getReg(), TRI, true); AR.isValid(); ++AR) {
      if (RegClass.contains(*AR)) {
        O << EpiphanyInstPrinter::getRegisterName(*AR);
        return false;
      }
    }
  }

  return true;
}

bool EpiphanyAsmPrinter::printSymbolicAddress(const MachineOperand &MO,
                                             bool PrintImmediatePrefix,
                                             StringRef Suffix, raw_ostream &O) {
  StringRef Name;
  StringRef Modifier;
  switch (MO.getType()) {
  default:
    llvm_unreachable("Unexpected operand for symbolic address constraint");
  case MachineOperand::MO_GlobalAddress:
    Name = Mang->getSymbol(MO.getGlobal())->getName();

    // Global variables may be accessed either via a GOT or in various fun and
    // interesting TLS-model specific ways. Set the prefix modifier as
    // appropriate here.
    if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(MO.getGlobal())) {
      Reloc::Model RelocM = TM.getRelocationModel();
		if (Subtarget->GVIsIndirectSymbol(GV, RelocM)) {
        Modifier = "got";
      }
    }
    break;
  case MachineOperand::MO_BlockAddress:
    Name = GetBlockAddressSymbol(MO.getBlockAddress())->getName();
    break;
  case MachineOperand::MO_ExternalSymbol:
    Name = MO.getSymbolName();
    break;
  case MachineOperand::MO_ConstantPoolIndex:
    Name = GetCPISymbol(MO.getIndex())->getName();
    break;
  }

  // Some instructions (notably ADRP) don't take the # prefix for
  // immediates. Only print it if asked to.
  if (PrintImmediatePrefix)
    O << '#';

  // Only need the joining "_" if both the prefix and the suffix are
  // non-null. This little block simply takes care of the four possibly
  // combinations involved there.
  if (Modifier == "" && Suffix == "")
    O << Name;
  else if (Modifier == "" && Suffix != "")
    O << ":" << Suffix << ':' << Name;
  else if (Modifier != "" && Suffix == "")
    O << ":" << Modifier << ':' << Name;
  else
    O << ":" << Modifier << '_' << Suffix << ':' << Name;

  return false;
}

bool EpiphanyAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNum,
                                        unsigned AsmVariant,
                                        const char *ExtraCode, raw_ostream &O) {
  const TargetRegisterInfo *TRI = MF->getTarget().getRegisterInfo();
  if (!ExtraCode || !ExtraCode[0]) {
    // There's actually no operand modifier, which leads to a slightly eclectic
    // set of behaviour which we have to handle here.
    const MachineOperand &MO = MI->getOperand(OpNum);
    switch (MO.getType()) {
    default:
      llvm_unreachable("Unexpected operand for inline assembly");
    case MachineOperand::MO_Register:
      // GCC prints the unmodified operand of a 'w' constraint as the vector
      // register. Technically, we could allocate the argument as a VPR128, but
      // that leads to extremely dodgy copies being generated to get the data
      // there.
      //if (printModifiedFPRAsmOperand(MO, TRI, Epiphany::VPR128RegClass, O))
      //  O << EpiphanyInstPrinter::getRegisterName(MO.getReg());
		llvm_unreachable("oh fuck.. mo 128vpr");
      break;
    case MachineOperand::MO_Immediate:
      O << '#' << MO.getImm();
      break;
    case MachineOperand::MO_FPImmediate:
      assert(MO.getFPImm()->isExactlyValue(0.0) && "Only FP 0.0 expected");
      O << "#0.0";
      break;
    case MachineOperand::MO_BlockAddress:
    case MachineOperand::MO_ConstantPoolIndex:
    case MachineOperand::MO_GlobalAddress:
    case MachineOperand::MO_ExternalSymbol:
      return printSymbolicAddress(MO, false, "", O);
    }
    return false;
  }

  // We have a real modifier to handle.
  switch(ExtraCode[0]) {
  default:
    // See if this is a generic operand
    return AsmPrinter::PrintAsmOperand(MI, OpNum, AsmVariant, ExtraCode, O);
  case 'c': // Don't print "#" before an immediate operand.
    if (!MI->getOperand(OpNum).isImm())
      return true;
    O << MI->getOperand(OpNum).getImm();
    return false;
  case 'w':
    // Output 32-bit general register operand, constant zero as wzr, or stack
    // pointer as wsp. Ignored when used with other operand types.
    return printModifiedGPRAsmOperand(MI->getOperand(OpNum), TRI,
                                      Epiphany::GPR32RegClass, O);
  //case 'x':
  //  // Output 64-bit general register operand, constant zero as xzr, or stack
  //  // pointer as sp. Ignored when used with other operand types.
  //  return printModifiedGPRAsmOperand(MI->getOperand(OpNum), TRI,
  //                                    Epiphany::GPR64RegClass, O);
  case 'H':
    // Output higher numbered of a 64-bit general register pair
  case 'Q':
    // Output least significant register of a 64-bit general register pair
  case 'R':
    // Output most significant register of a 64-bit general register pair

    // FIXME note: these three operand modifiers will require, to some extent,
    // adding a paired GPR64 register class. Initial investigation suggests that
    // assertions are hit unless it has a type and is made legal for that type
    // in ISelLowering. After that step is made, the number of modifications
    // needed explodes (operation legality, calling conventions, stores, reg
    // copies ...).
    llvm_unreachable("FIXME: Unimplemented register pairs");
  //case 'b':
  //  // Output 8-bit FP/SIMD scalar register operand, prefixed with b.
  //  return printModifiedFPRAsmOperand(MI->getOperand(OpNum), TRI,
  //                                    Epiphany::FPR8RegClass, O);
  //case 'h':
  //  // Output 16-bit FP/SIMD scalar register operand, prefixed with h.
  //  return printModifiedFPRAsmOperand(MI->getOperand(OpNum), TRI,
  //                                    Epiphany::FPR16RegClass, O);
  case 's':
    // Output 32-bit FP/SIMD scalar register operand, prefixed with s.
    return printModifiedFPRAsmOperand(MI->getOperand(OpNum), TRI,
                                      Epiphany::FPR32RegClass, O);
  //case 'd':
  //  // Output 64-bit FP/SIMD scalar register operand, prefixed with d.
  //  return printModifiedFPRAsmOperand(MI->getOperand(OpNum), TRI,
  //                                    Epiphany::FPR64RegClass, O);
  //case 'q':
  //  // Output 128-bit FP/SIMD scalar register operand, prefixed with q.
  //  return printModifiedFPRAsmOperand(MI->getOperand(OpNum), TRI,
  //                                    Epiphany::FPR128RegClass, O);
  case 'A':
    // Output symbolic address with appropriate relocation modifier (also
    // suitable for ADRP).
    return printSymbolicAddress(MI->getOperand(OpNum), false, "", O);
  case 'L':
    // Output bits 11:0 of symbolic address with appropriate :lo12: relocation
    // modifier.
    return printSymbolicAddress(MI->getOperand(OpNum), true, "lo12", O);
  case 'G':
    // Output bits 23:12 of symbolic address with appropriate :hi12: relocation
    // modifier (currently only for TLS local exec).
    return printSymbolicAddress(MI->getOperand(OpNum), true, "hi12", O);
  }


}

bool EpiphanyAsmPrinter::PrintAsmMemoryOperand(const MachineInstr *MI,
                                              unsigned OpNum,
                                              unsigned AsmVariant,
                                              const char *ExtraCode,
                                              raw_ostream &O) {
  // Currently both the memory constraints (m and Q) behave the same and amount
  // to the address as a single register. In future, we may allow "m" to provide
  // both a base and an offset.
  const MachineOperand &MO = MI->getOperand(OpNum);
  assert(MO.isReg() && "unexpected inline assembly memory operand");
  O << '[' << EpiphanyInstPrinter::getRegisterName(MO.getReg()) << ']';
  return false;
}

void EpiphanyAsmPrinter::PrintDebugValueComment(const MachineInstr *MI,
                                               raw_ostream &OS) {
  unsigned NOps = MI->getNumOperands();
  assert(NOps==4);
  OS << '\t' << MAI->getCommentString() << "DEBUG_VALUE: ";
  // cast away const; DIetc do not take const operands for some reason.
  DIVariable V(const_cast<MDNode *>(MI->getOperand(NOps-1).getMetadata()));
  OS << V.getName();
  OS << " <- ";
  // Frame address.  Currently handles register +- offset only.
  assert(MI->getOperand(0).isReg() && MI->getOperand(1).isImm());
  OS << '[' << EpiphanyInstPrinter::getRegisterName(MI->getOperand(0).getReg());
  OS << '+' << MI->getOperand(1).getImm();
  OS << ']';
  OS << "+" << MI->getOperand(NOps - 2).getImm();
}


#include "EpiphanyGenMCPseudoLowering.inc"

void EpiphanyAsmPrinter::EmitInstruction(const MachineInstr *MI) {
  // Do any auto-generated pseudo lowerings.
  if (emitPseudoExpansionLowering(OutStreamer, MI))
    return;

  switch (MI->getOpcode()) {
  case Epiphany::DBG_VALUE: {
    if (isVerbose() && OutStreamer.hasRawTextSupport()) {
      SmallString<128> TmpStr;
      raw_svector_ostream OS(TmpStr);
      PrintDebugValueComment(MI, OS);
      OutStreamer.EmitRawText(StringRef(OS.str()));
    }
    return;
  }
  }

  MCInst TmpInst;
  LowerEpiphanyMachineInstrToMCInst(MI, TmpInst, *this);
  OutStreamer.EmitInstruction(TmpInst);
}

void EpiphanyAsmPrinter::EmitEndOfAsmFile(Module &M) {
  if (Subtarget->isTargetELF()) {
    const TargetLoweringObjectFileELF &TLOFELF =
      static_cast<const TargetLoweringObjectFileELF &>(getObjFileLowering());

    MachineModuleInfoELF &MMIELF = MMI->getObjFileInfo<MachineModuleInfoELF>();

    // Output stubs for external and common global variables.
    MachineModuleInfoELF::SymbolListTy Stubs = MMIELF.GetGVStubList();
    if (!Stubs.empty()) {
      OutStreamer.SwitchSection(TLOFELF.getDataRelSection());
      const DataLayout *TD = TM.getDataLayout();

      for (unsigned i = 0, e = Stubs.size(); i != e; ++i) {
        OutStreamer.EmitLabel(Stubs[i].first);
        OutStreamer.EmitSymbolValue(Stubs[i].second.getPointer(),
                                    TD->getPointerSize(0), 0);
      }
      Stubs.clear();
    }
  }
}

bool EpiphanyAsmPrinter::runOnMachineFunction(MachineFunction &MF) {
  return AsmPrinter::runOnMachineFunction(MF);
}

// Force static initialization.
extern "C" void LLVMInitializeEpiphanyAsmPrinter() {
    RegisterAsmPrinter<EpiphanyAsmPrinter> X(TheEpiphanyTarget);
}

