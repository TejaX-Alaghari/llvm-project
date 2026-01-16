//===- AMDGPUDelayedMCExpr.h - Delayed MCExpr resolve -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_UTILS_AMDGPUDELAYEDMCEXPR_H
#define LLVM_LIB_TARGET_AMDGPU_UTILS_AMDGPUDELAYEDMCEXPR_H

#include "llvm/BinaryFormat/MsgPackDocument.h"
#include <deque>

namespace llvm {
class MCAssembler;
class MCExpr;

class DelayedMCExprs {
  struct Expr {
    msgpack::DocNode &DN;
    msgpack::Type Type;
    const MCExpr *ExprValue;
    Expr(msgpack::DocNode &DN, msgpack::Type Type, const MCExpr *ExprValue)
        : DN(DN), Type(Type), ExprValue(ExprValue) {}
  };

  std::deque<Expr> DelayedExprs;
  MCAssembler *Assembler = nullptr;

public:
  void setAssembler(MCAssembler *Asm) { Assembler = Asm; }
  bool resolveDelayedExpressions();
  void assignDocNode(msgpack::DocNode &DN, msgpack::Type Type,
                     const MCExpr *ExprValue);
  void clear();
  bool empty();
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_AMDGPU_UTILS_AMDGPUDELAYEDMCEXPR_H
