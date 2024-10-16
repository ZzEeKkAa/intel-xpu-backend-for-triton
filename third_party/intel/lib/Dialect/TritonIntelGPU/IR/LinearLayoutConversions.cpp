#include <vector>

#include "intel/include/Dialect/TritonIntelGPU/IR/Dialect.h"
#include "intel/include/Dialect/TritonIntelGPU/IR/LinearLayoutConversions.h"
#include "triton/Dialect/Triton/IR/Utility.h"
#include "triton/Dialect/TritonGPU/IR/Attributes.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Tools/LinearLayout.h"
#include "triton/Tools/StrUtil.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"

using namespace mlir::triton::gpu::intel;

namespace mlir::triton::gpu {
namespace {

// We use the following nomenclature in this file.
//
//  - ctaLayout: A layout for one block, i.e. input dims [register, lane, warp]
//    for register layouts, and input dims [offset] for shared layouts.
//  - cgaLayout: Arrangement of multiple blocks, i.e. input dims [block].
//
// Note that this is inconsistent with the type name CTALayoutAttr.  That type
// is equivalent to our cgaLayout.
//
// IMO the name CTALayoutAttr is wrong.  If we tried to be consistent anyway,
// then we'd have to rename ctaLayout to "warpLayout".  I think that's more
// confusing than being inconsistent about "cgaLayout", especially when we have
// to consider the size of the warpLayout (surely that's not the "warpSize").

#define S(v) StringAttr::get(ctx, (v))

// Returns ["out0", "out1", ..., "out<rank-1>"].
SmallVector<StringAttr> standardOutDimNames(MLIRContext *ctx, int rank) {
  SmallVector<StringAttr> ret;
  for (int i = 0; i < rank; i++) {
    ret.push_back(S("dim" + llvm::Twine(i)));
  }
  return ret;
}

// Returns a 1D -> ND layout that's equivalent to creating a 1D -> 1D mapping of
// size product(shape) and then reshaping to permute(shape, order).
LinearLayout identityND(StringAttr inDimName, ArrayRef<unsigned> shape,
                        ArrayRef<unsigned> order,
                        ArrayRef<StringAttr> outDimNames) {
  assert(shape.size() == order.size());

  MLIRContext *ctx = inDimName.getContext();
  LinearLayout ret = LinearLayout::empty();
  for (int i = 0; i < shape.size(); i++) {
    // Start with the most-minor dimension, which is order[0].
    int dim = order[i];
    ret *= LinearLayout::identity1D(shape[dim], inDimName, outDimNames[dim]);
  }
  return ret;
}

// Make a LinearLayout that maps a block-id to an N-dimensional index.
//
// The tensor is split up into CTAsPerCGA pieces, which are distributed among
// the CTAsPerCGA CTAs (i.e. blocks) in the CGA (i.e. groups).
//
// See the nomenclature note at the top of the file for an explanation of why
// this is called makeCgaLayout when it accepts a CTALayoutAttr.
LinearLayout makeCgaLayout(CTALayoutAttr layout) {
  MLIRContext *ctx = layout.getContext();
  StringAttr kBlock = S("block");

  int rank = layout.getCTAOrder().size();
  SmallVector<StringAttr> outDimNames = standardOutDimNames(ctx, rank);

  LinearLayout ret = LinearLayout::empty();
  for (int i = 0; i < rank; i++) {
    // Start with the most minor dimension, which is order[0].
    int dim = layout.getCTAOrder()[i];
    int split = layout.getCTASplitNum()[dim];
    int ctas = layout.getCTAsPerCGA()[dim];
    assert(ctas % split == 0);
    ret *= LinearLayout::identity1D(split, kBlock, outDimNames[dim]) *
           LinearLayout::zeros1D(ctas / split, kBlock, outDimNames[dim]);
  }

  // Transpose to standard order (dim0, dim1, ...).
  return ret.transposeOuts(outDimNames);
}

// Shrinks the output set of a layout function while leaving the input set
// unchanged, by making high-order inputs in inDimName map to the same output.
// Attempts to shrink down to desiredSize, but this is not always possible just
// by modifying one the specified input dimension.
//
// We do this by making the most-major inputs to the layout map to 0.  This
// effectively duplicates data along that input dimension.  For example, this
// layout has out-dim size 32:
//
//   L(register=1) = 8
//   L(register=2) = 4
//   L(register=4) = 1
//   L(lane=1) = 2
//   L(lane=2) = 16.
//
// If we shrink it to size 16 along the `lane` dimension, we set L(lane=2) to 0:
//
//   L(register=1) = 8
//   L(register=2) = 4
//   L(register=4) = 1
//   L(lane=1) = 2
//   L(lane=2) = 0.
//
// This means that lane=2 has the same data as lane=0.
//
// If we shrink to size 8 along the lane dimension, we set L(lane=1) = 0 as
// well.  But when we do this, we have to remove bit 1 (the value of L(lane=1))
// from all other bases:
//
//   L(register=1) = 4
//   L(register=2) = 2
//   L(register=1) = 1
//   L(lane=1) = 0
//   L(lane=2) = 0.
//
// Note this only works because the bases are powers of two.  I don't quite know
// what to do when they're not.
LinearLayout shrinkCodomain(const LinearLayout &layout, StringAttr inDimName,
                            StringAttr outDimName, int desiredSize) {
  assert(llvm::isPowerOf2_32(desiredSize));
  int outDimIdx = layout.getOutDimIndex(outDimName);
  int desiredZeros =
      llvm::Log2_32(layout.getOutDimSize(outDimName) / desiredSize);
  if (desiredZeros == 0) {
    return layout;
  }

  // Find the desiredZeros most-major basis vectors that are not already zero.
  // These are the ones we will set to zero.
  SmallVector<int> basesToZero;
  for (int i = layout.getInDimSizeLog2(inDimName) - 1;
       i >= 0 && basesToZero.size() < desiredZeros; i--) {
    int basis = layout.getBasis(inDimName, i, outDimName);
    if (basis != 0) {
      basesToZero.push_back(basis);
    }
  }

  // Bail if all the bases are already zero; nothing more we can do.
  if (basesToZero.empty()) {
    return layout;
  }

  // The algorithm below only works because the bases are powers of two.  I'm
  // not sure what to do otherwise.
  assert(llvm::all_of(basesToZero,
                      [&](int basis) { return llvm::isPowerOf2_32(basis); }));

  // We want to zero out the bases in `basesToZero`, and also "shift out" the
  // corresponding bits from all other bases.  For example if we remove the
  // basis with value 8 = 0b100, then if another basis has value 26 = 0b11010,
  // the 1 in its 3rd position gets removed and it becomes 10 = 0b1010.
  //
  // We could manually alter the bases in `layout` to achieve this, but it's
  // perhaps simpler to use the linearity of LLs to our advantage.
  //
  // Consider the function O which is the identity map from out-dims to
  // out-dims.  We can easily calculate what happens when we remove the relevant
  // bases from O.  Call this new function O'.
  //
  // Because of linearity, removing the bases from L is equivalent to composing
  // L with O'.  So that's what we do below.

  // Construct the out-dims -> out-dims identity layout O.
  LinearLayout outputIdentity = LinearLayout::empty();
  for (StringAttr dim : layout.getOutDimNames()) {
    outputIdentity *=
        LinearLayout::identity1D(layout.getOutDimSize(dim), dim, dim);
  }

  // Modify O to remove the relevant bases.
  //
  // TODO(jlebar): I don't like manually modifying bases here.  Perhaps this
  // should be a function on LinearLayout.
  LinearLayout::BasesT newBases = outputIdentity.getBases();
  llvm::sort(basesToZero);
  for (int basis : basesToZero) {
    int idx = llvm::Log2_32(basis);
    for (int i = newBases[outDimName].size() - 1; i > idx; i--) {
      newBases[outDimName][i][outDimIdx] =
          newBases[outDimName][i - 1][outDimIdx];
    }
    newBases[outDimName][idx][outDimIdx] = 0;
  }

  // Construct O'.
  LinearLayout transform(std::move(newBases),
                         llvm::to_vector(layout.getOutDimNames()));

  // Compose O' with L.
  return layout.compose(transform);
}

// For each out-dim d, ensure the layout's out-size (i.e. its codomain) is no
// larger than shape[d].  Do this without changing the size of the layout's
// inputs (i.e. leave its domain unchanged).
//
// This function is invariant to the order of the layout's input and output
// dimensions.
LinearLayout ensureLayoutNotLargerThan(
    const LinearLayout &layout,
    const llvm::SmallDenseMap<StringAttr, int64_t> &shape) {
  assert(shape.size() == layout.getNumOutDims());
  if (shape.empty()) {
    return layout;
  }
  MLIRContext *ctx = shape.begin()->first.getContext();

  // For the purposes of this function, "block" is the "most-minor" dimension.
  // This is just a consequence of how legacy layouts work: We only put the same
  // tensor element into two different blocks as a last resort, only after all
  // the registers in all the lanes in all the warps in a block already have the
  // same tensor element.  (Or, for shared layouts, only after all values in
  // smem within a block have the same value.)
  //
  // inDimNames combines the in dims for register and shared layouts; that's OK
  // because we skip in-dims that aren't present.  So we'll iterate over
  // {blocked, register, lane, warp} or {blocked, offset}.
  SmallVector<StringAttr> inDimNames = {
      // for both register and shared layouts
      S("block"),

      // for register layouts
      S("register"),
      S("lane"),
      S("warp"),

      // for shared layouts
      S("offset"),
  };

  LinearLayout ret = layout;
  for (auto outDimName : layout.getOutDimNames()) {
    int32_t actualSize = layout.getOutDimSize(outDimName);
    int32_t desiredSize = shape.lookup(outDimName);
    if (actualSize <= desiredSize) {
      continue;
    }
    assert(actualSize % desiredSize == 0);
    for (StringAttr inDimName : llvm::reverse(inDimNames)) {
      if (ret.hasInDim(inDimName)) {
        ret = shrinkCodomain(ret, inDimName, outDimName, desiredSize);
      }
    }
    assert(ret.getOutDimSize(outDimName) == desiredSize);
  }
  return ret;
}

// For each out-dim d, ensure the layout's out-size (i.e. its codomain) is no
// smaller than shape[d].  Do this by increasing the size of the layout's inputs
// along its most-minor dimension ("register" for register layouts, "offset" for
// shared layouts).
//
// This function is invariant to the order of the layout's input dimensions, but
// it cares about the order of the output dims, which should be minor-to-major.
LinearLayout ensureLayoutNotSmallerThan(
    const LinearLayout &layout,
    const llvm::SmallDenseMap<StringAttr, int64_t> &shape) {
  assert(shape.size() == layout.getNumOutDims());
  if (shape.empty()) {
    return layout;
  }

  MLIRContext *ctx = shape.begin()->first.getContext();
  StringAttr kDim = *layout.getInDimNames().begin();
  assert(kDim == "register" || kDim == "offset");

  LinearLayout ret = layout;
  for (StringAttr outDimName : layout.getOutDimNames()) {
    int32_t actualSize = layout.getOutDimSize(outDimName);
    int32_t desiredSize = shape.lookup(outDimName);
    assert(actualSize > desiredSize || desiredSize % actualSize == 0);
    ret *= LinearLayout::identity1D(desiredSize / actualSize, kDim, outDimName);
    assert(ret.getOutDimSize(outDimName) >= desiredSize);
  }
  return ret;
}

// Combines the layout of a CTA (input dims [register, lane, warp]) with the
// layout of a CGA (i.e. a block), and ensures that the resulting layout has the
// given shape.
//
// See the nomenclature note at the top of the file for why the variable with
// type CTALayoutAttr is called cgaLayoutAttr.
LinearLayout combineCtaCgaWithShape(LinearLayout ctaLayout,
                                    CTALayoutAttr cgaLayoutAttr,
                                    ArrayRef<int64_t> shape) {
  int rank = shape.size();
  assert(ctaLayout.getNumOutDims() == rank);
  assert(cgaLayoutAttr.getCTAOrder().size() == rank);
  MLIRContext *ctx = cgaLayoutAttr.getContext();

  SmallVector<StringAttr> outDimNames = standardOutDimNames(ctx, rank);

  llvm::SmallDenseMap<StringAttr, int64_t> labeledShape;
  for (auto [dim, size] : llvm::zip(outDimNames, shape)) {
    labeledShape[dim] = size;
  }

  LinearLayout cgaLayout =
      ensureLayoutNotLargerThan(makeCgaLayout(cgaLayoutAttr), labeledShape)
          .transposeOuts(llvm::to_vector(ctaLayout.getOutDimNames()));

  // Calculate the shape of the ctaLayout, which is `shape` divided by the
  // cgaLayout's size.
  llvm::SmallDenseMap<StringAttr, int64_t> ctaShape;
  assert(llvm::to_vector(ctaLayout.getOutDimNames()) ==
         llvm::to_vector(cgaLayout.getOutDimNames()));
  for (auto dim : ctaLayout.getOutDimNames()) {
    ctaShape[dim] =
        std::max(int64_t{1}, labeledShape[dim] / cgaLayout.getOutDimSize(dim));
  }

  ctaLayout = ensureLayoutNotSmallerThan(ctaLayout, ctaShape);
  ctaLayout = ensureLayoutNotLargerThan(ctaLayout, ctaShape);

  LinearLayout ret = (ctaLayout * cgaLayout).transposeOuts(outDimNames);
  for (auto dim : ret.getOutDimNames()) {
    assert(ret.getOutDimSize(dim) == labeledShape[dim]);
  }
  return ret;
}

} // anonymous namespace

// The layout example repeat_count=8, systolic_depth=8,
// execution_size=16 and operands_per_chan=2 for warp size 32.
// DPASInst layout of C operand:
//        execution size = 16
//<---------------------------------->
// t0  t1  t2  t3  ~ t12 t13 t14 t15          ^
// t16 t17 t18 t19 ~ t28 t29 t30 t31          |
// .   .   .   .   .   .   .   .   .          |
// .   .   .   .   .   .   .   .   .          | repeatCount = 8
// t0  t1  t2  t3  ~ t12 t13 t14 t15          |
// t16 t17 t18 t19 ~ t28 t29 t30 t31          v
// In this case, the LinearLayout bases are:
// Register:  {{2,0}, {4,0}}
// Lane:      {{0,1}, {0,2}, {0,4}, {0,8}, {1,0}}
// Currently, LinearLayout is not supported for DotOperandEncoding
// so only Operand C conversion is implemented.
std::vector<std::vector<int32_t>>
DPASRegBasesC(int repeatCount, int executionSize, int threadsPerWarp) {
  int rowsPerWarp = threadsPerWarp / executionSize;

  std::vector<std::vector<int32_t>> regBases;

  for (int rid = rowsPerWarp; rid < repeatCount; rid = rid * 2) {
    regBases.push_back({rid, 0});
  }

  return regBases;
}

std::vector<std::vector<int32_t>>
DPASLaneBasesC(int repeatCount, int executionSize, int threadsPerWarp) {

  std::vector<std::vector<int32_t>> laneBases;

  for (int tid = 1; tid < executionSize; tid = tid * 2) {
    laneBases.push_back({0, tid});
  }
  int rowsPerWarp = threadsPerWarp / executionSize;
  for (int row = 1; row < rowsPerWarp; row = row * 2) {
    laneBases.push_back({row, 0});
  }

  return laneBases;
}

std::optional<LinearLayout> DPAStoLinearLayout(ArrayRef<int64_t> shape,
                                               Attribute layout) {

  auto dpas = dyn_cast<DpasEncodingAttr>(layout);
  assert(dpas && "Must be DPAS Operand C layout");

  int rank = shape.size();
  assert(rank == dpas.getWarpsPerCTA().size());
  assert(rank == 2);

  MLIRContext *ctx = dpas.getContext();
  SmallVector<StringAttr> outDimNames = standardOutDimNames(ctx, rank);

  StringAttr kRegister = S("register");
  StringAttr kLane = S("lane");

  const SmallVector<unsigned> warpsPerCTA = dpas.getWarpsPerCTA();
  int threadsPerWarp = triton::gpu::getWarpSize(dpas);
  auto repCluster = dpas.getRepCluster();
  SmallVector<int64_t> numReps = dpas.getDPASRepetitions(shape, 2);

  auto tileLayout = LinearLayout::empty();
  int repeatCount = dpas.getRepeatCount();
  int executionSize = dpas.getExecutionSize();

  auto regBases = DPASRegBasesC(repeatCount, executionSize, threadsPerWarp);
  auto laneBases = DPASLaneBasesC(repeatCount, executionSize, threadsPerWarp);
  tileLayout =
      LinearLayout({{kRegister, regBases}, {kLane, laneBases}}, outDimNames);

  // The per-inst layout is repeated at each repCluster.
  // Hence, multiply with the identity layouts starting from the
  // least significant dimension.
  tileLayout *=
      LinearLayout::identity1D(repCluster[1], kRegister, outDimNames[1]);
  tileLayout *=
      LinearLayout::identity1D(repCluster[0], kRegister, outDimNames[0]);

  // Then, it is repeated by DPASRepetitions to form per-Warp layout.
  tileLayout *= LinearLayout::identity1D(numReps[1], kRegister, outDimNames[1]);
  tileLayout *= LinearLayout::identity1D(numReps[0], kRegister, outDimNames[0]);

  // Finally, per-warp layout is repeated among the warps in the CTA.
  LinearLayout warpLayout =
      identityND(S("warp"), dpas.getWarpsPerCTA(), {0, 1}, outDimNames);
  LinearLayout ctaLayout = tileLayout * warpLayout;

  return combineCtaCgaWithShape(ctaLayout, CTALayoutAttr::getDefault(ctx, rank),
                                shape);
}

} // namespace mlir::triton::gpu
