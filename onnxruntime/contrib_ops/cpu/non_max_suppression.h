// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/common/common.h"
#include "core/framework/op_kernel.h"
//#include "core/util/math_cpuonly.h"

namespace onnxruntime {
namespace contrib {

template <typename T>
class NonMaxSuppression final : public OpKernel {
 public:
  NonMaxSuppression(const OpKernelInfo& info) : OpKernel(info),
      pad_to_max_output_size_(info.GetAttrOrDefault<int64_t>("pad_to_max_output_size", 0)) {
    ORT_ENFORCE(info.GetAttr("max_output_size", &max_output_size_).IsOK());
    ORT_ENFORCE(info.GetAttr("iou_threshold", &iou_threshold_).IsOK());
    ORT_ENFORCE(iou_threshold_ >= 0 && iou_threshold_ <= 1, "iou_threshold must be in range [0, 1]");
    ORT_ENFORCE(info.GetAttr("score_threshold", &score_threshold_).IsOK());
  }

  Status Compute(OpKernelContext* context) const override;

private:
  bool SuppressByIOU(const T* boxes_data, int32_t box_index1, int32_t box_index2) const;
  void MaxMin(const T& lhs, const T& rhs, T& min, T& max) const;

private :
  int64_t max_output_size_;
  float iou_threshold_;
  float score_threshold_;
  int64_t pad_to_max_output_size_;
};
}  // namespace contrib
}  // namespace onnxruntime
