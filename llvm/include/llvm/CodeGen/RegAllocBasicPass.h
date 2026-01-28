//==- RegAllocBasicPass.h --- basic register allocator pass --------*-C++-*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CODEGEN_REGALLOC_BASIC_PASS_H
#define LLVM_CODEGEN_REGALLOC_BASIC_PASS_H

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/RegAllocCommon.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class RABasicPass : public PassInfoMixin<RABasicPass> {
public:
  struct Options {
    RegAllocFilterFunc Filter;
    StringRef FilterName;
    Options(RegAllocFilterFunc F = nullptr, StringRef FN = "all")
        : Filter(std::move(F)), FilterName(FN) {};
  };

  RABasicPass(Options Opts = Options()) : Opts(std::move(Opts)) {}
  PreservedAnalyses run(MachineFunction &MF, MachineFunctionAnalysisManager &AM);

  MachineFunctionProperties getRequiredProperties() const {
    return MachineFunctionProperties().setNoPHIs();
  }

  MachineFunctionProperties getClearedProperties() const {
    return MachineFunctionProperties().setIsSSA();
  }

  void
  printPipeline(raw_ostream &OS,
                function_ref<StringRef(StringRef)> MapClassName2PassName) const;
  static bool isRequired() { return true; }

private:
  Options Opts;
};

} // namespace llvm

#endif // LLVM_CODEGEN_REGALLOC_BASIC_PASS_H
