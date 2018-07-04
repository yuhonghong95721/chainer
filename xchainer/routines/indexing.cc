#include "xchainer/routines/indexing.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "nonstd/optional.hpp"

#include "xchainer/array.h"
#include "xchainer/array_index.h"
#include "xchainer/axes.h"
#include "xchainer/backward.h"
#include "xchainer/dtype.h"
#include "xchainer/graph.h"
#include "xchainer/macro.h"
#include "xchainer/routines/creation.h"
#include "xchainer/shape.h"
#include "xchainer/slice.h"
#include "xchainer/strides.h"

namespace xchainer {
namespace internal {
namespace {

// Returns an array where elements at indices are added by the addends `b`.
//
// It is not in-place  operation: the input arrays are not altered.
// It is differentiable with respect to `a` and `b`.
Array AddAt(const Array& a, const std::vector<ArrayIndex>& indices, const Array& b) {
    // TODO(sonots): dtype conversion
    CheckEqual(a.dtype(), b.dtype());

    Array out = a.AsConstant(CopyKind::kCopy);
    Array out_view = out.At(indices);

    // TODO(sonots): broadcasting
    CheckEqual(out_view.shape(), b.shape());

    a.device().Add(b, out_view, out_view);

    {
        BackwardBuilder bb{"add_at", out};
        if (a.IsBackpropRequired()) {
            bb.Define({a}, [](BackwardContext& bctx) { bctx.input_grad() = bctx.output_grad(); });
        }
        if (b.IsBackpropRequired()) {
            bb.Define({b}, [indices](BackwardContext& bctx) { bctx.input_grad() = bctx.output_grad().At(indices); });
        }
    }

    return out;
}

}  // namespace

Array At(const Array& a, const std::vector<ArrayIndex>& indices) {
    Shape out_shape{};
    Strides out_strides{};
    int64_t out_offset = a.offset();
    int64_t i_in = 0;
    for (const ArrayIndex& index : indices) {
        switch (index.tag()) {
            case ArrayIndexTag::kSingleElement: {
                int64_t dim = a.shape()[i_in];
                if (index.index() < -dim || dim <= index.index()) {
                    throw DimensionError{"Index ", index.index(), " is out of bounds for axis ", i_in, " with size ", dim};
                }
                out_offset += a.strides()[i_in] * ((index.index() + dim) % dim);
                ++i_in;
                break;
            }
            case ArrayIndexTag::kSlice: {
                const Slice& slice = index.slice();
                int64_t slice_length = slice.GetLength(a.shape()[i_in]);
                out_offset += a.strides()[i_in] * slice.GetStart(a.shape()[i_in]);
                out_shape.emplace_back(slice_length);
                out_strides.emplace_back(a.strides()[i_in] * slice.step());
                ++i_in;
                break;
            }
            case ArrayIndexTag::kNewAxis:
                out_shape.emplace_back(1);
                out_strides.emplace_back(0);
                break;
            default:
                XCHAINER_NEVER_REACH();
        }
    }
    for (int64_t i = i_in; i < a.ndim(); ++i) {
        out_shape.emplace_back(a.shape()[i]);
        out_strides.emplace_back(a.strides()[i]);
    }

    Array out = xchainer::internal::MakeArray(out_shape, out_strides, a.dtype(), a.device(), a.data(), out_offset);

    if (a.IsBackpropRequired()) {
        BackwardBuilder bb{"get_item", out};
        bb.Define({a}, [ indices, a_shape = a.shape(), a_dtype = a.dtype() ](BackwardContext & bctx) {
            const Array& gout = bctx.output_grad();
            Array gin = Zeros(a_shape, a_dtype, gout.device());
            bctx.input_grad() = AddAt(gin, indices, gout);
        });
    }

    return out;
}

}  // namespace internal

namespace {

// Adds elements of `b` indexed by `indices` into `a` and returns the result.
// Used in backward pass of Take()
//
// It is not in-place operation: the input arrays are not altered.
// It is differentiable with respect to `a` and `b`.
Array AddAt(const Array& a, const Array& indices, int8_t axis, const Array& b) {
    assert(0 <= axis && axis < a.ndim());
    assert(b.ndim() == indices.ndim() + a.ndim() - 1);
    CheckEqual(a.dtype(), b.dtype());

    Array out = EmptyLike(a, a.device());

    a.device().AddAt(a, indices, axis, b, out);

    {
        BackwardBuilder bb{"add_at", out};
        if (a.IsBackpropRequired()) {
            bb.Define({a}, [](BackwardContext& bctx) { bctx.input_grad() = bctx.output_grad(); });
        }
        if (b.IsBackpropRequired()) {
            bb.Define({b}, [indices, axis](BackwardContext& bctx) {
                assert(indices.IsConstant());
                bctx.input_grad() = Take(bctx.output_grad(), indices, axis);
            });
        }
    }

    return out;
}

}  // namespace

Array Take(const Array& a, const Array& indices, int8_t axis) {
    // TODO(niboshi): Support other dtypes by casting
    if (indices.dtype() != Dtype::kInt64) {
        throw DtypeError(
                std::string{"Only "} + GetDtypeName(Dtype::kInt64) + " is supported as indices, but given " +
                GetDtypeName(indices.dtype()));
    }

    int8_t axis_norm = internal::NormalizeAxis(axis, a.ndim());

    Shape out_shape{};
    std::copy(a.shape().begin(), a.shape().begin() + axis_norm, std::back_inserter(out_shape));
    std::copy(indices.shape().begin(), indices.shape().end(), std::back_inserter(out_shape));
    std::copy(a.shape().begin() + (axis_norm + 1), a.shape().end(), std::back_inserter(out_shape));
    Array out = Empty(out_shape, a.dtype(), a.device());

    a.device().Take(a, indices, axis_norm, out);

    if (a.IsBackpropRequired()) {
        BackwardBuilder bb{"take", out};
        bb.Define({a}, [ indices, axis_norm, a_shape = a.shape() ](BackwardContext & bctx) {
            const Array& gout = bctx.output_grad();
            bctx.input_grad() = AddAt(Zeros(a_shape, gout.dtype(), gout.device()), indices, axis_norm, gout);
        });
    }

    return out;
}

}  // namespace xchainer
