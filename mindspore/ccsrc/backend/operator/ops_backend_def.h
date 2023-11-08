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

#ifndef MINDSPORE_CCSRC_BACKEND_OPERATOR_OPS_BACKEND_DEF
#define MINDSPORE_CCSRC_BACKEND_OPERATOR_OPS_BACKEND_DEF

#include <memory>
#include "ir/anf.h"
#include "ir/primitive.h"

namespace mindspore::ops {
constexpr auto kNameDemo = "Demo";
constexpr auto kNameBatchNormWithActivation = "BatchNormWithActivation";
constexpr auto kNameBatchNormWithAddAndActivation = "BatchNormWithAddAndActivation";
constexpr auto kNameBatchNormGradWithActivation = "BatchNormGradWithActivation";
constexpr auto kNameBatchNormGradWithAddAndActivation = "BatchNormGradWithAddAndActivation";

GVAR_DEF(PrimitivePtr, kPrimDemo, std::make_shared<Primitive>(kNameDemo));
GVAR_DEF(PrimitivePtr, kPrimBatchNormWithActivation, std::make_shared<Primitive>(ops::kNameBatchNormWithActivation));
GVAR_DEF(PrimitivePtr, kPrimBatchNormWithAddAndActivation,
         std::make_shared<Primitive>(ops::kNameBatchNormWithAddAndActivation));
GVAR_DEF(PrimitivePtr, kPrimBatchNormGradWithActivation,
         std::make_shared<Primitive>(ops::kNameBatchNormGradWithActivation));
GVAR_DEF(PrimitivePtr, kPrimBatchNormGradWithAddAndActivation,
         std::make_shared<Primitive>(ops::kNameBatchNormGradWithAddAndActivation));
}  // namespace mindspore::ops
#endif  // MINDSPORE_CCSRC_BACKEND_OPERATOR_OPS_BACKEND_DEF
