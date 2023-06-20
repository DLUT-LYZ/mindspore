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

#include "src/extendrt/delegate/tensorrt/op/encoder_tensorrt.h"
#include <cuda_runtime.h>
#include <numeric>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include "mindspore/core/ops/nn_ops.h"
#include "src/extendrt/delegate/tensorrt/tensorrt_utils.h"
#include "NvInferRuntimeCommon.h"
#include "ops/encoder_layer.h"
#include "src/fastertransformer/kernels/unfused_attention_kernels.h"
#include "src/fastertransformer/kernels/activation_kernels.h"
#include "src/fastertransformer/utils/cuda_utils.h"
#include "src/fastertransformer/utils/allocator.h"
#include "src/fastertransformer/kernels/layernorm_kernels.h"
#include "src/extendrt/delegate/tensorrt/op/tensorrt_op.h"
#include "src/extendrt/delegate/tensorrt/distribution/distribution_base.h"
#include "src/extendrt/delegate/tensorrt/distribution/distribution_collective.h"
#include "mindspore/core/ops/op_name.h"

namespace mindspore::lite {
namespace {
constexpr std::size_t kTwo = 2;
}  // namespace

int EncoderTensorRT::unique_id_ = 0;
int EncoderTensorRT::IsSupport(const BaseOperatorPtr &base_operator, const std::vector<TensorInfo> &in_tensors,
                               const std::vector<TensorInfo> &out_tensors) {
  auto layer_norm = GetValue<bool>(base_operator->GetAttr(ops::kLayerNorm));
  auto position_bias = GetValue<bool>(base_operator->GetAttr(ops::kPositionBias1));
  auto use_past = GetValue<bool>(base_operator->GetAttr(ops::kUsePast));
  auto query_layer = GetValue<bool>(base_operator->GetAttr(ops::kQueryLayer));
  auto moe = GetValue<bool>(base_operator->GetAttr(ops::kMoe));
  size_t in_num = C8NUM;  // if mask=false in_num should be 6, mask default = true
  if (use_past) in_num += C4NUM;
  if (query_layer) in_num += C3NUM;
  if (moe) in_num += C1NUM;
  if (layer_norm) {
    if (position_bias)
      in_num += C1NUM;
    else
      in_num += C2NUM;
  }
  if (position_bias)
    in_num += C1NUM;
  else
    in_num += C6NUM;
  if (in_tensors.size() != in_num) {
    MS_LOG(ERROR) << "Unsupported input tensor size, size is " << in_tensors.size() << " and needs to be " << in_num;
    return RET_ERROR;
  }
  if (out_tensors.size() != C1NUM && out_tensors.size() != C3NUM) {
    MS_LOG(ERROR) << "Unsupported output tensor size, size is " << out_tensors.size();
    return RET_ERROR;
  }
  return RET_OK;
}

nvinfer1::ITensor *EncoderTensorRT::CastTensor(TensorRTContext *ctx, const TensorInfo &ms_tensor,
                                               const std::string &op_name) {
  if (ctx == nullptr || ctx->network() == nullptr) {
    MS_LOG(ERROR) << "context or network is null for ConvertConstantTensor";
    return nullptr;
  }
  nvinfer1::Dims dims = ConvertCudaDims(ms_tensor.Shape());
  if (dims.nbDims == -1) {
    MS_LOG(INFO) << ms_tensor.Name() << " ConvertCudaDims failed, convert as scalar.";
    dims.nbDims = 1;
    dims.d[0] = 1;
  }
  nvinfer1::DataType data_type = ConvertDataType(ms_tensor.DataType());
  if (!ms_tensor.IsConst()) {
    MS_LOG(ERROR) << "ConvertConstantTensor from a MSTensor with nullptr data: " << ms_tensor.Name();
    return nullptr;
  }
  nvinfer1::Weights weights{data_type, ms_tensor.Data(), ms_tensor.ElementNum()};
  if (data_type == nvinfer1::DataType::kFLOAT && runtime_->GetTransformerFfnFp16()) {
    void *data_float16 = malloc(ms_tensor.ElementNum() * sizeof(float));
    if (data_float16 == nullptr) {
      MS_LOG(ERROR) << "Malloc buffer failed.";
      return nullptr;
    }
    auto src = static_cast<const float *>(ms_tensor.Data());
    auto dst = static_cast<half *>(data_float16);
    for (int i = 0; i < ms_tensor.ElementNum(); i++) {
      dst[i] = static_cast<half>(src[i]);
    }
    weights.values = data_float16;
  }
  nvinfer1::IConstantLayer *constant_tensor = ctx->network()->addConstant(dims, weights);
  if (constant_tensor == nullptr) {
    MS_LOG(ERROR) << "create constant_tensor failed.";
    return nullptr;
  }
  ctx->RegisterLayer(constant_tensor, ms_tensor.Name() + "_" + op_name);
  auto tensor_ptr = constant_tensor->getOutput(0);
  return tensor_ptr;
}

int EncoderTensorRT::AddVsl(int encoder_input_idx, int input_number, TensorRTContext *ctx,
                            nvinfer1::ITensor **inputTensors, const char *name) {
  if (runtime_->GetVslEncoderPluginId() == -1) {
    auto vsl_plugin = std::make_shared<VslCompressPlugin>(name, device_id_);
    CHECK_NULL_RETURN(vsl_plugin);
    nvinfer1::ITensor *inputVsl = ctx->network()->getInput(encoder_input_idx);
    auto vsl_compress_layer = ctx->network()->addPluginV2(&inputVsl, C1NUM, *vsl_plugin);
    if (vsl_compress_layer == nullptr) {
      MS_LOG(ERROR) << " create vsl compress layer failed for: ";
      return RET_ERROR;
    }
    auto plugin_id = static_cast<int>(ctx->network()->getNbLayers() - 1);
    runtime_->SetVslEncoderPluginId(plugin_id);
    vsl_compress_layer->setName("plugin_vsl_compress");
    nvinfer1::ITensor *vsl_output_tensor = vsl_compress_layer->getOutput(0);
    ctx->RegisterTensor(ITensorHelper{vsl_output_tensor, Format::NCHW, true}, "vsl_compress_output");
    inputTensors[input_number] = vsl_output_tensor;
  } else {
    auto vsl_compress_layer = ctx->network()->getLayer(runtime_->GetVslEncoderPluginId());
    inputTensors[input_number] = vsl_compress_layer->getOutput(0);
  }
  return RET_OK;
}

int EncoderTensorRT::InitParam(fastertransformer::encoderParamRun *params) {
  auto encoder_op = AsOps<ops::EncoderLayer>();
  if (encoder_op == nullptr) {
    MS_LOG(ERROR) << "op action convert failed";
    return RET_ERROR;
  }
  cublasHandle_t cublas_handle = GetCublasHandle();
  // update commonparam
  params->common_param.eft = false;
  params->common_param.cublas_handle = cublas_handle;
  params->common_param.head_num = encoder_op->get_head_num();
  params->common_param.head_size = encoder_op->get_head_size();
  params->common_param.rank_id = 0;
  params->common_param.rank_num = 1;
#ifdef LITE_CUDA_DISTRIBUTION
  params->common_param.rank_id = GetRankID();
  params->common_param.rank_num = GetGPUGroupSize();
#endif
  params->ffn_param.is_moe = encoder_op->get_moe();
  params->common_param.use_past = encoder_op->get_use_past();
  params->common_param.query_layer = encoder_op->get_query_layer();
  // connect commonparam to attention and ffn
  params->common_param.hidden_size =
    params->common_param.head_num * params->common_param.head_size * params->common_param.rank_num;
  // update encoder_param_
  params->encoder.is_layernorm = encoder_op->get_layer_norm();
  params->encoder.layernorm_post = encoder_op->get_post_layernorm();
  params->encoder.eps1 = encoder_op->get_eps_layernorm1();
  params->encoder.eps2 = encoder_op->get_eps_layernorm2();
  params->encoder.eps3 = encoder_op->get_eps_layernorm3();
  params->ffn_param.ffn_param.ffn_hidden_size = encoder_op->get_ffn_hidden_size();
  params->ffn_param.ffn_param.expert_num = encoder_op->get_expert_num();
  params->ffn_param.ffn_param.expert_offset = encoder_op->get_expert_offset_id();
  params->ffn_param.ffn_param.capacity_factor = encoder_op->get_capacity_factor();
  params->ffn_param.ffn_param.ffn_fp16 = runtime_->GetTransformerFfnFp16();
  params->ffn_param.ffn_param.has_beta = !params->attn.attn.position_bias;
  params->attn.attn.is_cross = params->common_param.query_layer ? true : false;
  params->attn.attn.position_bias = encoder_op->get_position_bias();
  params->attn.attn.projection_bias = !params->attn.attn.position_bias;
  params->attn.attn.qkv_bias = !params->attn.attn.position_bias;
  params->encoder.has_beta = !params->attn.attn.position_bias;
  params->ffn_param.ffn_param.ffn_bias = !params->attn.attn.position_bias;
  params->attn.attn.mask = true;
  if (encoder_op->get_act_type() == ActType::ActType_Gelu) {
    params->ffn_param.ffn_param.act_type = fastertransformer::ActType_Gelu;
  } else if (encoder_op->get_act_type() == ActType::ActType_Relu) {
    params->ffn_param.ffn_param.act_type = fastertransformer::ActType_Relu;
  } else if (encoder_op->get_act_type() == ActType::ActType_No) {
    params->ffn_param.ffn_param.act_type = fastertransformer::ActType_No;
  } else {
    params->ffn_param.ffn_param.act_type = static_cast<fastertransformer::ActType>(encoder_op->get_act_type());
  }
  params->attn.attn.scale = encoder_op->get_scale();
  return RET_OK;
}

void EncoderTensorRT::CastFfnTensors(fastertransformer::encoderParamRun *params, TensorRTContext *ctx) {
  size_t start_fp16 = (params->encoder.layernorm_post) ? C7NUM : C9NUM;
  size_t end_fp16 = (params->encoder.layernorm_post) ? C11NUM : C13NUM;
  if (params->attn.attn.position_bias) {
    start_fp16 = C6NUM;
    end_fp16 = C9NUM;
  }
  for (size_t i = 0; i < in_tensors_.size(); i++) {
    auto in_tensor = input(ctx, i);
    if (in_tensors_[i].IsConst() || in_tensor.trt_tensor_ == nullptr) {
      if (i > start_fp16 && i < end_fp16) {
        in_tensor.trt_tensor_ = CastTensor(ctx, in_tensors_[i], op_name_);
        ctx->RegisterTensor(in_tensor, in_tensors_[i].Name());
      } else {
        in_tensor.trt_tensor_ = lite::ConvertConstantTensor(ctx, in_tensors_[i], op_name_);
        ctx->RegisterTensor(in_tensor, in_tensors_[i].Name());
      }
    }
  }
}

void EncoderTensorRT::BuildEncoderTensors(TensorRTContext *ctx) {
  for (size_t i = 0; i < in_tensors_.size(); i++) {
    auto in_tensor = input(ctx, i);
    if (in_tensors_[i].IsConst() || in_tensor.trt_tensor_ == nullptr) {
      in_tensor.trt_tensor_ = lite::ConvertConstantTensor(ctx, in_tensors_[i], op_name_);
      ctx->RegisterTensor(in_tensor, in_tensors_[i].Name());
    }
  }
}

void EncoderTensorRT::BuildUsePastTensors(TensorRTContext *ctx) {
  for (size_t i = 0; i < in_tensors_.size(); i++) {
    auto in_tensor = input(ctx, i);
    if (in_tensors_[i].IsConst() || in_tensor.trt_tensor_ == nullptr) {
      if (i == C1NUM || i == C2NUM) {
        int *data = const_cast<int *>(static_cast<const int *>(in_tensors_[i].Data()));
        *data = unique_id_ | (i << C8NUM);
      }
      in_tensor.trt_tensor_ = lite::ConvertConstantTensor(ctx, in_tensors_[i], op_name_);
      ctx->RegisterTensor(in_tensor, in_tensors_[i].Name());
    }
  }
}

int EncoderTensorRT::AddInnerOp(TensorRTContext *ctx) {
  if (ctx == nullptr || ctx->network() == nullptr) {
    MS_LOG(ERROR) << "context or network is invalid";
    return RET_ERROR;
  }
  fastertransformer::encoderParamRun params;
  if (InitParam(&params) != RET_OK) {
    MS_LOG(ERROR) << "Init param in encoder tensorrt failed.";
    return RET_ERROR;
  }
  int encoder_input_idx = runtime_->GetTransformerEncoderInputIdx();
  if (IsWeightInputHanledInner()) {
    if (params.common_param.use_past) {
      BuildUsePastTensors(ctx);
    } else if (IsFfnMixPrecision()) {
      CastFfnTensors(&params, ctx);
    } else {
      BuildEncoderTensors(ctx);
    }
  }

  nvinfer1::ITensor *input_tensor = input(ctx, 0).trt_tensor_;
  const int input_number = inputs().size();
  const int vsl_input_number = (encoder_input_idx == -1) ? 0 : 1;
  nvinfer1::ITensor *inputTensors[input_number + vsl_input_number];
  for (int i = 0; i < input_number; i++) {
    inputTensors[i] = input(ctx, i).trt_tensor_;
  }
  if (encoder_input_idx != -1 && params.common_param.use_past == false) {
    params.common_param.eft = true;
    AddVsl(encoder_input_idx, input_number, ctx, inputTensors, input_tensor->getName());
  }
  auto compute_type = runtime_->GetRuntimePrecisionMode();
  auto plugin = std::make_shared<EncoderPlugin>(input_tensor->getName(), compute_type, params, device_id_);
  nvinfer1::IPluginV2Layer *encoder_layer =
    ctx->network()->addPluginV2(inputTensors, (input_number + vsl_input_number), *plugin);

  if (encoder_layer == nullptr) {
    MS_LOG(ERROR) << "add encoder op failed for TensorRT.";
    return RET_ERROR;
  }
  encoder_layer->setName((op_name_ + "plugin_encoder").c_str());
  nvinfer1::ITensor *encoder_tensor = encoder_layer->getOutput(0);
  ctx->RegisterTensor(ITensorHelper{encoder_tensor, Format::NCHW, true}, out_tensors_[0].Name());
  this->layer_ = encoder_layer;
  unique_id_++;
  return RET_OK;
}

REGISTER_TENSORRT_PLUGIN(EncoderPluginCreater);
template class TensorRTPluginCreater<EncoderPlugin>;
template <class T>
nvinfer1::PluginFieldCollection TensorRTPluginCreater<T>::field_collection_{};
template <class T>
std::vector<nvinfer1::PluginField> TensorRTPluginCreater<T>::fields_;

int EncoderPlugin::enqueue(const nvinfer1::PluginTensorDesc *inputDesc, const nvinfer1::PluginTensorDesc *outputDesc,
                           const void *const *inputs, void *const *outputs, void *workspace,
                           cudaStream_t stream) noexcept {
  if (compute_type_ == RuntimePrecisionMode_FP16) {
    return RunCudaEncoder<half>(inputDesc, outputDesc, inputs, outputs, workspace, stream,
                                CUBLAS_GEMM_DEFAULT_TENSOR_OP);
  } else {
    return RunCudaEncoder<float>(inputDesc, outputDesc, inputs, outputs, workspace, stream,
                                 CUBLAS_GEMM_DEFAULT_TENSOR_OP);
  }
}

#ifdef LITE_CUDA_DISTRIBUTION
int allGatherFunc(const void *input_addr, void *output_addr, size_t count, nvinfer1::DataType data_type,
                  cudaStream_t stream) {
  return DistributionCollective::instance().AllGatherWrapper(input_addr, output_addr, count, data_type, stream,
                                                             NCCL_WORLD_GROUP);
}

int allReduceSumFunc(const void *input_addr, void *output_addr, size_t count, nvinfer1::DataType data_type,
                     cudaStream_t stream) {
  return DistributionCollective::instance().AllReduceWrapper(input_addr, output_addr, count, data_type, Reduce_Sum,
                                                             stream, NCCL_WORLD_GROUP);
}
#endif

template <typename T>
int EncoderPlugin::RunCudaEncoder(const nvinfer1::PluginTensorDesc *inputDesc,
                                  const nvinfer1::PluginTensorDesc *outputDesc, const void *const *inputs,
                                  void *const *outputs, void *workspace, cudaStream_t stream, cublasGemmAlgo_t algoId) {
  params_.common_param.algo = algoId;
  params_.common_param.stream = stream;
  params_.common_param.all_gather_func = nullptr;
  params_.common_param.all_reduce_sum_func = nullptr;
#ifdef LITE_CUDA_DISTRIBUTION
  if (params_.common_param.rank_num > 1) {
    params_.common_param.all_gather_func = allGatherFunc;
    params_.common_param.all_reduce_sum_func = allReduceSumFunc;
  }
#endif
  void *inputs_forward[num_of_inputs_];
  for (int i = 0; i < num_of_inputs_; i++) {
    inputs_forward[i] = const_cast<void *>(inputs[i]);
  }
  void *outputs_forward[] = {outputs[0]};
  fastertransformer::forwardEncoder<T>(inputs_forward, num_of_inputs_, outputs_forward, num_of_outputs_, &params_,
                                       workspace);
  return RET_OK;
}

bool EncoderPlugin::supportsFormatCombination(int pos, const nvinfer1::PluginTensorDesc *tensorsDesc, int nbInputs,
                                              int nbOutputs) noexcept {
  auto type = (compute_type_ == RuntimePrecisionMode_FP16) ? nvinfer1::DataType::kHALF : nvinfer1::DataType::kFLOAT;

  if (params_.common_param.eft && pos == (nbInputs - C1NUM)) {
    bool res = (tensorsDesc[pos].type == nvinfer1::DataType::kINT32) ? true : false;
    return res;
  }
  if (params_.common_param.use_past && pos == (nbInputs - C1NUM)) {
    bool res = (tensorsDesc[pos].type == nvinfer1::DataType::kINT32) ? true : false;
    return res;
  }

  if (params_.common_param.use_past && pos == (nbInputs - C2NUM)) {
    bool res = (tensorsDesc[pos].type == nvinfer1::DataType::kINT32) ? true : false;
    return res;
  }

  if (params_.ffn_param.is_moe) {
    int expert_id = C7NUM;
    if (params_.common_param.query_layer)
      expert_id++;
    else if (params_.encoder.is_layernorm)
      expert_id += C2NUM;
    if (pos == (nbInputs - expert_id)) {
      bool res = (tensorsDesc[pos].type == nvinfer1::DataType::kINT32) ? true : false;
      return res;
    }
  }

  bool res = (tensorsDesc[pos].format == nvinfer1::TensorFormat::kLINEAR) && (tensorsDesc[pos].type == type);
  return res;
}

void EncoderPlugin::configurePlugin(const nvinfer1::DynamicPluginTensorDesc *in, int nbInputs,
                                    const nvinfer1::DynamicPluginTensorDesc *out, int nbOutputs) noexcept {
  int request_batch_size = static_cast<int>(in[0].desc.dims.d[0]);
  int request_src_seq_len = static_cast<int>(in[0].desc.dims.d[1]);
  int embedding_size = -1;
  if (params_.common_param.query_layer) {
    request_batch_size = C1NUM;
    request_src_seq_len = static_cast<int>(in[0].desc.dims.d[0]);
    embedding_size = static_cast<int>(in[nbInputs - C3NUM].desc.dims.d[0]);
  }
  int request_tgt_seq_len = request_src_seq_len;
  params_.common_param.batch_size = request_batch_size;
  params_.common_param.src_seq_len = request_src_seq_len;
  params_.common_param.tgt_seq_len = request_tgt_seq_len;
  params_.common_param.h_token_num = request_batch_size * request_src_seq_len;
  params_.common_param.embedding_size = embedding_size;
  num_of_inputs_ = nbInputs;
  num_of_outputs_ = nbOutputs;
}

size_t EncoderPlugin::getWorkspaceSize(const nvinfer1::PluginTensorDesc *inputs, int nbInputs,
                                       const nvinfer1::PluginTensorDesc *outputs, int nbOutputs) const noexcept {
  if (compute_type_ == RuntimePrecisionMode_FP16) {
    return fastertransformer::GetEncoderLayerWorkspaceSize<half>(&params_);
  } else {
    return fastertransformer::GetEncoderLayerWorkspaceSize<float>(&params_);
  }
}

nvinfer1::DimsExprs EncoderPlugin::getOutputDimensions(int32_t index, const nvinfer1::DimsExprs *inputs,
                                                       int nbInputDims, nvinfer1::IExprBuilder &exprBuilder) noexcept {
  nvinfer1::DimsExprs dims;
  if (index == 0) {
    if (params_.encoder.is_layernorm) {
      int num_dims = C2NUM;
      dims.nbDims = num_dims;
      for (int i = 0; i < num_dims; i++) {
        dims.d[i] = exprBuilder.constant(inputs[index].d[i + 1]->getConstantValue());
      }
    } else if (params_.common_param.query_layer) {
      int num_dims = inputs[0].nbDims;
      dims.nbDims = num_dims;
      for (int i = 0; i < num_dims - 1; i++) {
        dims.d[i] = exprBuilder.constant(inputs[index].d[i]->getConstantValue());
      }
      int embeeding_id = nbInputDims - C3NUM;
      dims.d[num_dims - 1] = exprBuilder.constant(inputs[embeeding_id].d[0]->getConstantValue());
    } else {
      int num_dims = inputs[0].nbDims;
      dims.nbDims = num_dims;
      for (int i = 0; i < num_dims; i++) {
        dims.d[i] = exprBuilder.constant(inputs[index].d[i]->getConstantValue());
      }
    }
  }
  return dims;
}

nvinfer1::IPluginV2DynamicExt *EncoderPlugin::clone() const noexcept {
  auto *plugin = new EncoderPlugin(*this);
  if (plugin == nullptr) {
    MS_LOG(ERROR) << "plugin is null";
    return nullptr;
  }
  plugin->setPluginNamespace(name_space_.c_str());
  plugin->params_.attn.common_param = &plugin->params_.common_param;
  plugin->params_.ffn_param.common_param = &plugin->params_.common_param;
  return plugin;
}

size_t EncoderPlugin::getSerializationSize() const noexcept {
  return sizeof(int) + sizeof(fastertransformer::encoderParamRun);
}

void EncoderPlugin::serialize(void *buffer) const noexcept {
  SerializeValue(&buffer, &compute_type_, sizeof(int));
  SerializeValue(&buffer, &params_, sizeof(fastertransformer::encoderParamRun));
}
REGISTER_TENSORRT_CREATOR(ops::kNameEncoderLayer, EncoderTensorRT)
}  // namespace mindspore::lite
