/**
 * Copyright 2023 Huawei Technologies Co., Ltd
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

#include "src/extendrt/graph_compiler/single_graph_scheduler.h"
#include "src/common/log_util.h"
#include "src/common/ops/operator_populate/operator_populate_register.h"
#include "src/common/tensor_util.h"
#include "src/extendrt/graph_compiler/infershape_helper.h"
#include "src/extendrt/kernel/kernel_lib.h"
#include "src/extendrt/kernel/kernel_selector/kernel_selector.h"
#include "src/litert/kernel_registry.h"
#include "src/litert/lite_kernel.h"
#include "src/litert/pass/format_pass/format_pass.h"
#include "tools/optimizer/graph/node_infershape.h"

namespace mindspore {
namespace lite {
InferKernel *SingleGraphScheduler::Schedule(const CompileResultPtr &node_list) {
  // infer shape
  auto infer_ret = lite::FallBackInferShape(node_list, compile_option_->format, context_.get());
  if (infer_ret != RET_OK && infer_ret != lite::RET_INFER_INVALID) {
    MS_LOG(ERROR) << "InferShape CompileResult node failed.";
    return nullptr;
  }

  execution_flow_ = std::make_shared<infer::ExecutionFlow>();
  MSLITE_CHECK_PTR_RETURN(execution_flow_, nullptr);
  execution_flow_->SetInputs(node_list->GetInputs());
  execution_flow_->SetOutputs(node_list->GetOutputs());
  execution_flow_->SetTensors(node_list->GetTensors());
  execution_flow_->SetContext(context_);
  auto schedule_ret = SelectKernel(node_list);
  if (schedule_ret != lite::RET_OK) {
    MS_LOG(ERROR) << "Scheduler CompileResult to kernels failed.";
    return nullptr;
  }

  // fp16/fp32 weight, transpose weight
  auto cast_ret = HandleWeightForKernels();
  if (!cast_ret) {
    MS_LOG(ERROR) << "Handle weight for kernels failed.";
    return nullptr;
  }

  // append kernel with transpose
  auto kernel = execution_flow_->ConstructFusionKernel();
  if (kernel == nullptr) {
    MS_LOG(ERROR) << "Construct subgraph kernel failed.";
    return nullptr;
  }

  std::vector<kernel::KernelExec *> subkernels = {kernel};
  auto ret = OptimizeTranspose(&subkernels);
  if (ret != kSuccess) {
    MS_LOG(ERROR) << "Optimize format of executionplan failed.";
    return nullptr;
  }

  return kernel;
}

int SingleGraphScheduler::SelectKernel(const CompileResultPtr &node_list) {
  kernel_selector_ = kernel::CreateKernelSelector(compile_option_);
  std::vector<InferKernel *> kernels;
  for (const auto &node : node_list->GetNodes()) {
    MSLITE_CHECK_PTR_RETURN(node, lite::RET_NULL_PTR);
    auto kernel_exec =
      kernel_selector_->CreateKernel({node->GetType(), node->GetKernelAttr(), compile_option_->format,
                                      compile_option_->backend, node->GetBaseOperator(), node->GetCNode()},
                                     node->GetInputs(), node->GetOutputs(), context_.get());
    if (kernel_exec == nullptr) {
      MS_LOG(ERROR) << "Create kernel exec for node: " << node->GetName() << " failed.";
      return lite::RET_NOT_SUPPORT;
    }
    kernel_exec->set_name(node->GetName());
    kernels.push_back(kernel_exec);
  }
  execution_flow_->SetKernels(kernels);
  return lite::RET_OK;
}

bool SingleGraphScheduler::HandleWeightForKernels() {
  if (compile_option_->datatype != kNumberTypeFloat32 && compile_option_->datatype != kNumberTypeFloat16) {
    return true;
  }
  auto kernels = execution_flow_->GetKernels();
  for (const auto &kernel : kernels) {
    for (const auto &input : kernel->in_tensors()) {
      // only cast const tensor
      if (!input->IsConst()) {
        continue;
      }
      // only support fp32->fp16 or fp16->fp32
      if (input->data_type() != kNumberTypeFloat32 && input->data_type() != kNumberTypeFloat16) {
        continue;
      }
      auto ret = CastConstTensorData(input, compile_option_->datatype, context_->device_and_pkg_support_fp16_);
      if (ret != lite::RET_OK) {
        MS_LOG(ERROR) << "Cast data for tensor: " << input->tensor_name() << " failed.";
        return false;
      }
    }
  }
  return true;
}

Status SingleGraphScheduler::OptimizeTranspose(std::vector<InferKernel *> *kernels) {
  auto tensors = execution_flow_->GetTensors();
  auto ret = lite::pass::DoFormatPass(kernels, &tensors, compile_option_->format);
  if (ret != RET_OK) {
    MS_LOG(INFO) << "Run Optimize transpose pass failed.";
    return kLiteError;
  }
  return kSuccess;
}
}  // namespace lite
}  // namespace mindspore
