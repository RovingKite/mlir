//===- Utils.cpp - Utilities to support the Linalg dialect ----------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements utilities for the Linalg dialect.
//
//===----------------------------------------------------------------------===//

#include "mlir/Linalg/Utils/Utils.h"
#include "mlir/EDSC/Helpers.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Linalg/IR/LinalgOps.h"
#include "mlir/Linalg/IR/LinalgTypes.h"
#include "mlir/Linalg/Passes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/STLExtras.h"

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace mlir::linalg;
using namespace llvm;

mlir::edsc::LoopNestRangeBuilder::LoopNestRangeBuilder(
    ArrayRef<ValueHandle *> ivs, ArrayRef<ValueHandle> ranges) {
  for (unsigned i = 0, e = ranges.size(); i < e; ++i) {
    assert(ranges[i].getType() && "expected !linalg.range type");
    assert(ranges[i].getValue()->getDefiningOp() &&
           "need operations to extract range parts");
    auto rangeOp = cast<RangeOp>(ranges[i].getValue()->getDefiningOp());
    auto lb = rangeOp.min();
    auto ub = rangeOp.max();
    // This must be a constexpr index until we relax the affine.for constraint
    auto step =
        cast<ConstantIndexOp>(rangeOp.step()->getDefiningOp()).getValue();
    loops.emplace_back(ivs[i], ValueHandle(lb), ValueHandle(ub), step);
  }
  assert(loops.size() == ivs.size() && "Mismatch loops vs ivs size");
}

mlir::edsc::LoopNestRangeBuilder::LoopNestRangeBuilder(
    ArrayRef<ValueHandle *> ivs, ArrayRef<Value *> ranges)
    : LoopNestRangeBuilder(
          ivs, SmallVector<ValueHandle, 4>(ranges.begin(), ranges.end())) {}

ValueHandle LoopNestRangeBuilder::LoopNestRangeBuilder::operator()(
    std::function<void(void)> fun) {
  if (fun)
    fun();
  for (auto &lit : reverse(loops)) {
    lit({});
  }
  return ValueHandle::null();
}

SmallVector<Value *, 8> mlir::getRanges(Operation *op) {
  SmallVector<Value *, 8> res;
  if (auto view = dyn_cast<ViewOp>(op)) {
    res.append(view.getIndexings().begin(), view.getIndexings().end());
  } else if (auto slice = dyn_cast<SliceOp>(op)) {
    for (auto *i : slice.getIndexings())
      if (i->getType().isa<RangeType>())
        res.push_back(i);
  } else {
    for (auto *v : op->getOperands()) {
      if (v->getType().isa<ViewType>()) {
        if (auto *vOp = v->getDefiningOp()) {
          auto tmp = getRanges(vOp);
          res.append(tmp.begin(), tmp.end());
        } else {
          llvm_unreachable("Needs an operation to extract ranges from a view");
        }
      }
    }
  }
  return res;
}

// Implementation details:
//   1. Checks whether `ranges` define a new View by performing an equality
//      check between the range ssa-values and the operands of
//      `viewDefiningOp`.
//   2. If all ranges happen to be equal, op creation is elided and the
//      original result is returned instead.
//   3. Otherwise, creates a SliceOp with the new `ranges`.
// This is used to abstract away the creation of a SliceOp.
Value *mlir::createOrReturnView(FuncBuilder *b, Location loc,
                                Operation *viewDefiningOp,
                                ArrayRef<Value *> ranges) {
  if (auto view = dyn_cast<ViewOp>(viewDefiningOp)) {
    auto indexings = view.getIndexings();
    if (std::equal(indexings.begin(), indexings.end(), ranges.begin()))
      return view.getResult();
    return b->create<SliceOp>(loc, view.getResult(), ranges);
  }
  auto slice = cast<SliceOp>(viewDefiningOp);
  unsigned idxRange = 0;
  SmallVector<Value *, 4> newIndexings;
  bool elide = true;
  for (auto indexing : slice.getIndexings()) {
    if (indexing->getType().isa<RangeType>()) {
      elide &= (indexing != ranges[idxRange]);
      newIndexings.push_back(ranges[idxRange++]);
    } else
      newIndexings.push_back(indexing);
  }
  if (elide)
    return slice.getResult();
  return b->create<SliceOp>(loc, slice.getBaseView(), newIndexings);
}

Value *mlir::extractRangePart(Value *range, RangePart part) {
  assert(range->getType().isa<RangeType>() && "expected range type");
  if (range->getDefiningOp()) {
    if (auto r = dyn_cast_or_null<RangeOp>(range->getDefiningOp())) {
      switch (part) {
      case RangePart::Min:
        return r.min();
      case RangePart::Max:
        return r.max();
      case RangePart::Step:
        return r.step();
      }
    }
  }
  llvm_unreachable("need operations to extract range parts");
}
// Folding eagerly is necessary to abide by affine.for static step requirement.
// We must propagate constants on the steps as aggressively as possible.
// Returns nullptr if folding is not trivially feasible.
static Value *tryFold(AffineMap map, ArrayRef<Value *> operands,
                      FunctionConstants &state) {
  assert(map.getNumResults() == 1 && "single result map expected");
  auto expr = map.getResult(0);
  if (auto dim = expr.dyn_cast<AffineDimExpr>())
    return operands[dim.getPosition()];
  if (auto sym = expr.dyn_cast<AffineSymbolExpr>())
    return operands[map.getNumDims() + sym.getPosition()];
  if (auto cst = expr.dyn_cast<AffineConstantExpr>())
    return state.getOrCreateIndex(cst.getValue());
  return nullptr;
}

static Value *emitOrFoldComposedAffineApply(FuncBuilder *b, Location loc,
                                            AffineMap map,
                                            ArrayRef<Value *> operandsRef,
                                            FunctionConstants &state) {
  SmallVector<Value *, 4> operands(operandsRef.begin(), operandsRef.end());
  fullyComposeAffineMapAndOperands(&map, &operands);
  if (auto *v = tryFold(map, operands, state))
    return v;
  return b->create<AffineApplyOp>(loc, map, operands);
}

SmallVector<Value *, 4> mlir::applyMapToRangePart(FuncBuilder *b, Location loc,
                                                  AffineMap map,
                                                  ArrayRef<Value *> ranges,
                                                  RangePart part,
                                                  FunctionConstants &state) {
  SmallVector<Value *, 4> rangeParts(ranges.size());

  llvm::transform(ranges, rangeParts.begin(),
                  [&](Value *range) { return extractRangePart(range, part); });

  SmallVector<Value *, 4> res;
  res.reserve(map.getNumResults());
  unsigned numDims = map.getNumDims();
  // For each `expr` in `map`, applies the `expr` to the values extracted from
  // ranges. If the resulting application can be folded into a Value*, the
  // folding occurs eagerly. Otherwise, an affine.apply operation is emitted.
  for (auto expr : map.getResults()) {
    AffineMap map = AffineMap::get(numDims, 0, expr, {});
    res.push_back(
        emitOrFoldComposedAffineApply(b, loc, map, rangeParts, state));
  }
  return res;
}

Value *FunctionConstants::getOrCreateIndex(int64_t v) {
  auto it = map.find(v);
  if (it != map.end())
    return it->second;
  FuncBuilder builder(f);
  edsc::ScopedContext s(builder, f.getLoc());
  return map.insert(std::make_pair(v, edsc::intrinsics::constant_index(v)))
      .first->getSecond();
}
