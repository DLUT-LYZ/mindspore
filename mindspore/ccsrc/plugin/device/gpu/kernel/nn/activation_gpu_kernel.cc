/**
 * Copyright 2019-2022 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "plugin/device/gpu/kernel/nn/activation_gpu_kernel.h"
#include <memory>
#include "mindspore/core/ops/nn_optimizer_ops.h"
#include "mindspore/core/ops/math_ops.h"
#include "mindspore/core/ops/gen_ops_name.h"
#include "mindspore/core/ops/op_utils.h"

namespace mindspore {
namespace kernel {
std::map<std::string, std::vector<std::pair<KernelAttr, ActivationFwdGpuKernelMod::ActivationFunc>>>
  ActivationFwdGpuKernelMod::kernel_attr_map_ = {
    {ops::kNameReLU6,
     {{KernelAttr().AddInputAttr(kNumberTypeFloat32).AddOutputAttr(kNumberTypeFloat32),
       &ActivationFwdGpuKernelMod::LaunchKernel<float>},
      {KernelAttr().AddInputAttr(kNumberTypeFloat16).AddOutputAttr(kNumberTypeFloat16),
       &ActivationFwdGpuKernelMod::LaunchKernel<half>}}},
    {ops::kNameElu,
     {{KernelAttr().AddInputAttr(kNumberTypeFloat64).AddOutputAttr(kNumberTypeFloat64),
       &ActivationFwdGpuKernelMod::LaunchKernel<double>},
      {KernelAttr().AddInputAttr(kNumberTypeFloat32).AddOutputAttr(kNumberTypeFloat32),
       &ActivationFwdGpuKernelMod::LaunchKernel<float>},
      {KernelAttr().AddInputAttr(kNumberTypeFloat16).AddOutputAttr(kNumberTypeFloat16),
       &ActivationFwdGpuKernelMod::LaunchKernel<half>}}},
};

bool ActivationFwdGpuKernelMod::Init(const BaseOperatorPtr &base_operator, const std::vector<KernelTensorPtr> &inputs,
                                     const std::vector<KernelTensorPtr> &outputs) {
  kernel_name_ = base_operator->name();
  cudnn_handle_ = device::gpu::GPUDeviceManager::GetInstance().GetCudnnHandle();

  auto iter = kernel_attr_map_.find(kernel_name_);
  if (iter == kernel_attr_map_.end()) {
    MS_LOG(ERROR)
      << "For 'Activation', the kernel name must be in "
      << kernel::Map2Str<std::map, std::vector<std::pair<KernelAttr, ActivationFwdGpuKernelMod::ActivationFunc>>>(
           kernel_attr_map_)
      << ", but got " << kernel_name_;
    return false;
  }
  auto kernel_attr = GetKernelAttrFromTensors(inputs, outputs);
  auto [is_match, index] = MatchKernelAttr(kernel_attr, GetOpSupport());
  if (!is_match) {
    MS_LOG(ERROR) << "For '" << kernel_name_ << "', it does not support this kernel data type: " << kernel_attr;
    return false;
  }
  kernel_func_ = kernel_attr_map_.at(kernel_name_)[index].second;

  static const std::map<std::string, cudnnActivationMode_t> activation_mode_map = {
    {ops::kNameReLU6, CUDNN_ACTIVATION_CLIPPED_RELU}, {ops::kNameElu, CUDNN_ACTIVATION_ELU}};
  auto mode_iter = activation_mode_map.find(kernel_name_);
  if (mode_iter == activation_mode_map.end()) {
    MS_LOG(ERROR) << "For '" << kernel_name_ << "', only support these activations: "
                  << kernel::Map2Str<std::map, cudnnActivationMode_t>(activation_mode_map) << ", but got "
                  << kernel_name_;
    return KRET_RESIZE_FAILED;
  }
  mode_ = mode_iter->second;

  dtype_ = inputs.at(kIndex0)->dtype_id();
  return true;
}

int ActivationFwdGpuKernelMod::Resize(const BaseOperatorPtr &base_operator, const std::vector<KernelTensorPtr> &inputs,
                                      const std::vector<KernelTensorPtr> &outputs,
                                      const std::map<uint32_t, tensor::TensorPtr> &) {
  if (int ret = KernelMod::Resize(base_operator, inputs, outputs); ret != KRET_OK) {
    return ret;
  }
  size_t input_num = inputs.size();
  if (input_num != 1) {
    MS_LOG(ERROR) << "For '" << kernel_name_ << "', the number of inputs must be 1, but got " << input_num;
    return KRET_RESIZE_FAILED;
  }
  input_shape_ = inputs.at(kIndex0)->GetShapeVector();
  is_null_input_ = CHECK_NULL_INPUT(input_shape_);
  if (is_null_input_) {
    return KRET_OK;
  }

  CHECK_CUDNN_RET_WITH_EXCEPT_NOTRACE(cudnnCreateTensorDescriptor(&data_descriptor_),
                                      "For 'Activation', cudnnCreateTensorDescriptor failed.");
  CHECK_CUDNN_RET_WITH_EXCEPT_NOTRACE(cudnnCreateActivationDescriptor(&activation_desc_),
                                      "For 'Activation', cudnnCreateActivationDescriptor failed.");
  cudnn_data_type_ = GetCudnnDataType(TypeIdLabel(inputs.at(kIndex0)->dtype_id()));
  CheckTensorSize({input_shape_});
  ShapeVector shape;
  double coef = (mode_ == CUDNN_ACTIVATION_CLIPPED_RELU) ? 6.0 : 0.0;
  if (mode_ == CUDNN_ACTIVATION_ELU) {
    auto alpha = inputs[kIndex1]->GetValueWithCheck<float>();
    coef = static_cast<double>(alpha);
  }
  CHECK_CUDNN_RET_WITH_EXCEPT_NOTRACE(
    cudnnSetActivationDescriptor(activation_desc_, mode_, CUDNN_NOT_PROPAGATE_NAN, coef),
    "For 'Activation', cudnnSetActivationDescriptor failed.");
  const int split_dim = 4;
  if (input_shape_.size() <= split_dim) {
    ShapeNdTo4d(input_shape_, &shape);
    if (inputs.at(kIndex0)->format() == mindspore::Format::NHWC) {
      CHECK_CUDNN_RET_WITH_EXCEPT_NOTRACE(
        cudnnSetTensor4dDescriptor(data_descriptor_, CUDNN_TENSOR_NHWC, cudnn_data_type_, LongToInt(shape[0]),
                                   LongToInt(shape[3]), LongToInt(shape[1]), LongToInt(shape[2])),
        "For 'Activation', cudnnSetTensor4dDescriptor failed.");
    } else {
      CHECK_CUDNN_RET_WITH_EXCEPT_NOTRACE(
        cudnnSetTensor4dDescriptor(data_descriptor_, CUDNN_TENSOR_NCHW, cudnn_data_type_, LongToInt(shape[0]),
                                   LongToInt(shape[1]), LongToInt(shape[2]), LongToInt(shape[3])),
        "For 'Activation', cudnnSetTensor4dDescriptor failed.");
    }
  } else {
    CudnnSetTensorNdDescriptor(input_shape_, data_descriptor_, cudnn_data_type_, kernel_name_);
  }
  return KRET_OK;
}

std::vector<KernelAttr> ActivationFwdGpuKernelMod::GetOpSupport() {
  auto iter = kernel_attr_map_.find(kernel_name_);
  if (iter == kernel_attr_map_.end()) {
    MS_LOG(ERROR)
      << "For 'Activation', the kernel name must be in "
      << kernel::Map2Str<std::map, std::vector<std::pair<KernelAttr, ActivationFwdGpuKernelMod::ActivationFunc>>>(
           kernel_attr_map_)
      << ", but got " << kernel_name_;
    return std::vector<KernelAttr>{};
  }
  std::vector<KernelAttr> support_list;
  (void)std::transform(iter->second.begin(), iter->second.end(), std::back_inserter(support_list),
                       [](const std::pair<KernelAttr, ActivationFunc> &item) { return item.first; });
  return support_list;
}

template <typename T>
bool ActivationFwdGpuKernelMod::LaunchKernel(const std::vector<kernel::KernelTensor *> &inputs,
                                             const std::vector<kernel::KernelTensor *> &outputs) {
  T *input = GetDeviceAddress<T>(inputs, kIndex0);
  T *output = GetDeviceAddress<T>(outputs, kIndex0);

  if constexpr (std::is_same_v<T, double>) {
    constexpr double alpha = 1.0;
    constexpr double beta = 0.0;
    CHECK_CUDNN_RET_WITH_EXCEPT_NOTRACE(
      cudnnActivationForward(cudnn_handle_, activation_desc_, &alpha, data_descriptor_, input, &beta, data_descriptor_,
                             output),
      "For 'Activation', cudnnActivationForward failed.");
  } else {
    constexpr float alpha = 1.0;
    constexpr float beta = 0.0;
    CHECK_CUDNN_RET_WITH_EXCEPT_NOTRACE(
      cudnnActivationForward(cudnn_handle_, activation_desc_, &alpha, data_descriptor_, input, &beta, data_descriptor_,
                             output),
      "For 'Activation', cudnnActivationForward failed.");
  }

  return true;
}

MS_KERNEL_FACTORY_REG_BY_CREATOR(NativeGpuKernelMod, ReLU6,
                                 []() { return std::make_shared<ActivationFwdGpuKernelMod>(ops::kNameReLU6); });
MS_KERNEL_FACTORY_REG_BY_CREATOR(NativeGpuKernelMod, Elu,
                                 []() { return std::make_shared<ActivationFwdGpuKernelMod>(ops::kNameElu); });
}  // namespace kernel
}  // namespace mindspore
