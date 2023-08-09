/**
 * Copyright 2022 Huawei Technologies Co., Ltd
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

#include "src/litert/pass/format_pass/format_pass.h"
#include "src/litert/pass/format_pass/insert_transpose.h"
#include "src/litert/pass/format_pass/eliminate_transpose.h"
#include "src/common/draw/drawer.h"

namespace mindspore::lite::pass {
int FormatOptimize::AddPass(FormatPassPtr pass) {
  CHECK_NULL_RETURN(pass);
  pass_list_.push_back(pass);
  return RET_OK;
}

int FormatOptimize::RunPass(kernel::SubGraphKernel *graph, std::vector<Tensor *> *tensors) {
  for (FormatPassPtr pass : pass_list_) {
    CHECK_NULL_RETURN(pass);

    auto status = pass->RunPass(graph, tensors);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "Run pass failed";
      return status;
    }
    DrawDot(graph, pass->name());
  }
  return RET_OK;
}

int DoFormatPass(std::vector<mindspore::kernel::KernelExec *> *subgraph_list,
                 std::vector<mindspore::lite::Tensor *> *tensors, mindspore::Format graph_format) {
  for (const auto &subgraph : *subgraph_list) {
    FormatOptimizePtr optimize = std::make_shared<FormatOptimize>();

    optimize->AddPass(std::make_shared<InsertTranspose>(graph_format));
    optimize->AddPass(std::make_shared<EliminateTranspose>(graph_format));

    auto graph = reinterpret_cast<kernel::SubGraphKernel *>(subgraph);
    auto ret = optimize->RunPass(graph, tensors);
    if (ret != RET_OK) {
      MS_LOG(ERROR) << "Runtime format pass failed.";
      return RET_ERROR;
    }
  }

  return RET_OK;
}

int RuntimeFormatPass(std::vector<mindspore::kernel::KernelExec *> *subgraph_list,
                      std::vector<mindspore::lite::Tensor *> *tensors, mindspore::Format graph_format) {
#ifndef ENABLE_MULTI_LAYOUT
  return RET_OK;
#else
  return DoFormatPass(subgraph_list, tensorsc, graph_format);
#endif
}
}  // namespace mindspore::lite::pass
