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
#include "frontend/expander/pack/packfunc.h"

#include <unordered_map>
#include <vector>
#include <memory>
#include "mindspore/core/ops/framework_ops.h"
#include "include/common/debug/anf_ir_dump.h"
#include "frontend/expander/pack/pack_expander.h"
#include "utils/ms_context.h"
#include "abstract/ops/primitive_infer_map.h"
#include "mindspore/core/ops/packfunc.h"

namespace mindspore {
namespace expander {
namespace {
bool IsAbstractDynamicShape(const std::vector<AbstractBasePtr> &input_args) {
  return std::any_of(input_args.begin(), input_args.end(),
                     [](const AbstractBasePtr &abs) { return abs->BuildShape()->IsDynamic(); });
}

bool IsAbstractOutputTensor(const AbstractBasePtr &abs) {
  if (abs->isa<abstract::AbstractTuple>()) {
    auto abs_tuple = abs->cast<abstract::AbstractTuplePtr>()->elements();
    return std::all_of(abs_tuple.begin(), abs_tuple.end(),
                       [](const AbstractBasePtr &abs) { return IsAbstractOutputTensor(abs); });
  }
  return abs->isa<abstract::AbstractTensor>();
}
}  // namespace
using PackGraphMap = std::unordered_map<abstract::AbstractBasePtrList, FuncGraphPtr,
                                        abstract::AbstractBasePtrListHasher, abstract::AbstractBasePtrListEqual>;

static std::unordered_map<std::string, PackGraphMap> pack_graph_cache;
void ClearAllCache() { pack_graph_cache.clear(); }

FuncGraphPtr ExpandPackFunc(const PrimitivePtr &prim, const abstract::AbstractBasePtrList &abs_list) {
  auto key = GetValue<std::string>(prim->GetAttr("unique_key"));
  PackExpander::is_pynative_mode = GetValue<bool>(prim->GetAttr("is_pynative_mode"));
  auto &graph_map = pack_graph_cache[key];
  auto it = graph_map.find(abs_list);
  if (it != graph_map.end()) {
    return it->second;
  }
  auto prim_py = prim->cast_ptr<PrimitivePy>();
  MS_EXCEPTION_IF_NULL(prim_py);
  auto expander = expander::PackExpander::Instance();
  FuncGraphPtr graph;
  {
    py::gil_scoped_acquire acquire;
    py::object expand_func = prim_py->GetPyObj().attr("__expand__");
    py::object inputs = expander->BeginGraph(abs_list);
    py::object output = expand_func(inputs);
    graph = expander->EndGraph(output);
    graph_map[abs_list] = graph;
  }
  static bool dump_result = (common::GetEnv("MS_DEV_DUMP_PACK") == "on");
  if (dump_result) {
    DumpIR("pack_func_" + key + ".ir", graph, true);
  }
  return graph;
}

class PackFuncInfer : public abstract::OpInferBase {
 public:
  BaseShapePtr InferShape(const PrimitivePtr &primitive,
                          const std::vector<AbstractBasePtr> &input_args) const override {
    auto abs = InferShapeAndType(nullptr, primitive, input_args);
    return abs->BuildShape();
  }

  TypePtr InferType(const PrimitivePtr &primitive, const std::vector<AbstractBasePtr> &input_args) const override {
    auto abs = InferShapeAndType(nullptr, primitive, input_args);
    return abs->BuildType();
  }

  AbstractBasePtr InferShapeAndType(const abstract::AnalysisEnginePtr &engine, const PrimitivePtr &primitive,
                                    const std::vector<AbstractBasePtr> &input_args) const override {
    if (IsAbstractDynamicShape(input_args)) {
      MS_LOG(WARNING) << "Dynamic shape operator is not fully supported in trace graph capturing. Please check the "
                         "dump-ir to confirm its correctness.";
    }
    auto graph = ExpandPackFunc(primitive, input_args);
    MS_EXCEPTION_IF_NULL(graph);
    // the python primitive object may be used in different places with different inputs, so we
    // cannot save the graph in graph mode. But for pynative mode, this primitive is inferred
    // in forward thread sequentially and deep copied to backend runtime, so we can save graph
    // in attr to save performance.
    auto abs = graph->output()->abstract();
    if (PackExpander::is_pynative_mode) {
      primitive->set_attr("recent_graph", graph);
      if (!IsAbstractOutputTensor(abs)) {
        MS_EXCEPTION(ValueError)
          << "The output of trace captured graph should be one or more flattened Tensor, bug get "
          << abs->BuildType()->ToString() << ".";
      }
    }
    return abs;
  }
};
}  // namespace expander
namespace ops {
REGISTER_PRIMITIVE_OP_INFER_IMPL(PackFunc, prim::kPrimPackFunc, expander::PackFuncInfer, false);
}  // namespace ops
}  // namespace mindspore
