// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/common/common.h"
#include "core/common/exceptions.h"
#include "core/framework/op_kernel.h"
#include "core/providers/cpu/nn/autopad_type.h"
#include "core/util/math.h"

namespace onnxruntime {

// helper function
template <bool ForceSymmetricAutoPadding>
Status ComputePadAndOutputShape(
    const int64_t in_dim,
    const int64_t stride,
    const int64_t kernel,
    const int64_t dilation,
    AutoPadType pad_type,
    int64_t* pad_head,
    int64_t* pad_tail,
    int64_t* out_dim) {
  const int64_t dkernel = dilation * (kernel - 1) + 1;

  if (pad_type == AutoPadType::NOTSET) {
    *out_dim = static_cast<int64_t>(static_cast<float>(in_dim + *pad_head + *pad_tail - dkernel) / stride + 1);
  } else {
    switch (pad_type) {
      case AutoPadType::VALID:
        *pad_head = 0;
        *pad_tail = 0;
        *out_dim = (in_dim - dkernel) / stride + 1;
        break;
      case AutoPadType::SAME_UPPER:
      case AutoPadType::SAME_LOWER: {
        ORT_ENFORCE(dilation == 1, "Dilation not supported for AutoPadType::SAME_UPPER or AutoPadType::SAME_LOWER.");
        int64_t legacy_target_size = (in_dim + stride - 1) / stride;
        int64_t pad_needed = (legacy_target_size - 1) * stride + kernel - in_dim;
        *out_dim = (in_dim + pad_needed - dkernel) / stride + 1;

        // make sure padding is symmetric
        if (ForceSymmetricAutoPadding)
          pad_needed = math::roundUpPow2<int64_t, 2>(pad_needed);

        if (pad_type == AutoPadType::SAME_LOWER) {
          *pad_head = (pad_needed + 1) / 2;
        } else {
          *pad_head = pad_needed / 2;
        }
        *pad_tail = pad_needed - *pad_head;
      } break;
      default:
        return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "pad type not supported.");
    }
  }
  return Status::OK();
}

// base class used by Conv and ConvTranspose
class ConvBase {
 protected:
  ConvBase(const OpKernelInfo& info) {
    std::string auto_pad;
    auto status = info.GetAttr<std::string>("auto_pad", &auto_pad);
    auto_pad_ = status.IsOK() ? StringToAutoPadType(auto_pad) : AutoPadType::NOTSET;

    kernel_shape_specified_ = info.GetAttrs<int64_t>("kernel_shape", kernel_shape_).IsOK();

    status = info.GetAttrs<int64_t>("strides", strides_);
    if (!status.IsOK()) {
      strides_.resize(kernel_shape_.size(), 1);
    }

    status = info.GetAttrs<int64_t>("pads", pads_);
    if (!status.IsOK()) {
      pads_.resize(kernel_shape_.size() * 2, 0);
    }

    status = info.GetAttrs<int64_t>("dilations", dilations_);
    if (!status.IsOK()) {
      dilations_.resize(kernel_shape_.size(), 1);
    }

    status = info.GetAttr<int64_t>("group", &group_);
    if (!status.IsOK()) {
      group_ = 1;
    }

#if false
    // TODO: Re-enable when attributes values are guaranteed to be filled.
    std::string auto_pad;
    ORT_ENFORCE(info.GetAttr<std::string>("auto_pad", &auto_pad).IsOK());
    auto_pad_ = StringToAutoPadType(auto_pad);
    ORT_ENFORCE(info.GetAttr<int64_t>("group", &group_).IsOK());
    ORT_ENFORCE(info.GetAttrs<int64_t>("kernel_shape", kernel_shape_).IsOK());
    ORT_ENFORCE(info.GetAttrs<int64_t>("strides", strides_).IsOK());
    ORT_ENFORCE(info.GetAttrs<int64_t>("pads", pads_).IsOK());
    ORT_ENFORCE(info.GetAttrs<int64_t>("dilations", dilations_).IsOK());
#endif
  }

  ~ConvBase() {}

 protected:
  std::vector<int64_t> ComputeKernelShape(const TensorShape& weight_shape) const {
    if (kernel_shape_specified_)
      return kernel_shape_;
    else {
      auto& weight_dims = weight_shape.GetDims();
      std::vector<int64_t> result(weight_dims.begin() + 2, weight_dims.end());
      return result;
    }
  }

  Status ValidateInputShape(const Tensor* X, const Tensor* W) const {
    const int64_t C = X->Shape()[1];
    const int64_t M = W->Shape()[0];

    if (X->Shape().NumDimensions() != W->Shape().NumDimensions()) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "X num_dims does not match W num_dims.",
                                     " X: ", X->Shape().ToString().c_str(),
                                     " W: ", W->Shape().ToString().c_str());
    }

    if (C != W->Shape()[1] * group_) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Input channels C is not equal to kernel channels * group.",
                                     " C: ", C,
                                     " kernel channels: ", W->Shape()[1],
                                     " group: ", group_);
    }

    if (M % group_ != 0) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Output channels M is not divisible by group.",
                                     " M: ", M,
                                     " group: ", group_);
    }
    return Status::OK();
  }

  template <bool ForceSymmetricAutoPadding = false>
  Status InferOutputShape(const TensorShape& input_shape,
                          const std::vector<int64_t>& kernel_shape,
                          const std::vector<int64_t>& strides,
                          const std::vector<int64_t>& dilations,
                          std::vector<int64_t>* pads,
                          std::vector<int64_t>* output_shape) const {
    int rank = gsl::narrow_cast<int>(input_shape.NumDimensions());
    for (int dim = 0; dim < rank; ++dim) {
      if (dim >= strides.size() || dim >= kernel_shape.size() ||
          dim >= dilations.size() || dim >= pads->size() ||
          rank + dim >= pads->size()) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Out of bound access to array");
      }
      int64_t dim_size = 0;
      ORT_RETURN_IF_ERROR(ComputePadAndOutputShape<ForceSymmetricAutoPadding>(
          input_shape[dim],
          strides[dim],
          kernel_shape[dim],
          dilations[dim],
          auto_pad_,
          &pads->at(dim),
          &pads->at(input_shape.NumDimensions() + dim),
          &dim_size));
      if (dim_size <= 0) {
        return Status(common::ONNXRUNTIME, common::INVALID_ARGUMENT, "Invalid input shape: " + input_shape.ToString());
      }
      output_shape->push_back(dim_size);
    }
    return Status::OK();
  }

  AutoPadType auto_pad_;
  int64_t group_;
  bool kernel_shape_specified_;
  std::vector<int64_t> strides_;
  std::vector<int64_t> pads_;
  std::vector<int64_t> dilations_;
  std::string activation_;
  float alpha_;

 private:
  std::vector<int64_t> kernel_shape_;  // must use ComputeKernelShape(...), instead of kernel_shape_
};

}  // namespace onnxruntime
