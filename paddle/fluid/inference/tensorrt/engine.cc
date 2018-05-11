/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/inference/tensorrt/engine.h"

#include <NvInfer.h>
#include <cuda.h>
#include <glog/logging.h>
#include <string>
#include "paddle/fluid/inference/tensorrt/helper.h"
#include "paddle/fluid/platform/enforce.h"

namespace paddle {
namespace inference {
namespace tensorrt {

void TensorRTEngine::Build(const DescType& paddle_model) {
  PADDLE_ENFORCE(false, "not implemented");
}

void TensorRTEngine::BuildFromONNX(const std::string& model_dir,
                                   const std::string& model_file) {
  infer_builder_.reset(createInferBuilder(logger_));
  infer_ptr<nvonnxparser::IOnnxConfig> config(createInferBuilder(logger_));
  config->setModelFileName(AbsPath(model_dir, model_file).c_str());
  infer_ptr<nvonnxparser::IONNXParser> parser(createONNXParser(*config));
  if (!parser->parse(AbsPath(model_dir, model_file)).c_str(),
      nvinfer1::DataType::kFLOAT) {
    logger_.log(nvinfer1::ILogger::Severity::kERROR,
                "failed to parse ONNX file");
    exit(EXIT_FAILURE);
  }
  if (!parser->convertToTRTNetwork()) {
    logger_.log(nvinfer1::ILogger::Severity::kERROR,
                "ERROR, failed to convert ONNX network into TRT network.");
    exit(EXIT_FAILURE);
  }
  infer_network_.reset(parser->getTRTNetwork());
  // Get input and output.
  for (int i = 0; i < infer_network_->getNbInputs(); i++) {
    auto* in = infer_network_->getInput(i);
    auto dims = AccumDims(in->getDimensions());
  }
}

void TensorRTEngine::Execute(int batch_size) {
  // TODO(Superjomn) consider to make buffers not a temp variable and resuable.
  std::vector<void*> buffers;
  for (auto& buf : buffers_) {
    PADDLE_ENFORCE_NOT_NULL(buf.buffer, "buffer should be allocated");
    PADDLE_ENFORCE_GT(buf.max_size, 0);
    PADDLE_ENFORCE(buf.device == DeviceType::GPU);
    buffers.push_back(buf.buffer);
  }
  infer_context_->enqueue(batch_size, buffers.data(), *stream_, nullptr);
  cudaStreamSynchronize(*stream_);
}

TensorRTEngine::~TensorRTEngine() {
  // clean buffer
  for (auto& buf : buffers_) {
    if (buf.buffer != nullptr) {
      PADDLE_ENFORCE_EQ(0, cudaFree(buf.buffer));
      buf.buffer = nullptr;
      buf.max_size = 0;
    }
  }
}

void TensorRTEngine::FreezeNetwork() {
  PADDLE_ENFORCE(infer_builder_ != nullptr,
                 "Call InitNetwork first to initialize network.");
  PADDLE_ENFORCE(infer_network_ != nullptr,
                 "Call InitNetwork first to initialize network.");
  // build engine.
  infer_builder_->setMaxBatchSize(max_batch_);
  infer_builder_->setMaxWorkspaceSize(max_workspace_);

  infer_engine_.reset(infer_builder_->buildCudaEngine(*infer_network_));
  PADDLE_ENFORCE(infer_engine_ != nullptr, "build cuda engine failed!");

  infer_context_.reset(infer_engine_->createExecutionContext());

  // allocate GPU buffers.
  buffers_.resize(buffer_sizes_.size());
  for (auto& item : buffer_sizes_) {
    if (item.second == 0) {
      auto slot_offset = infer_engine_->getBindingIndex(item.first.c_str());
      item.second = kDataTypeSize[static_cast<int>(
                        infer_engine_->getBindingDataType(slot_offset))] *
                    AccumDims(infer_engine_->getBindingDimensions(slot_offset));
    }
    auto& buf = buffer(item.first);
    CHECK(buf.buffer == nullptr);  // buffer should be allocated only once.
    PADDLE_ENFORCE_EQ(0, cudaMalloc(&buf.buffer, item.second));
    buf.size = buf.max_size = item.second;
    buf.device = DeviceType::GPU;
  }
}

nvinfer1::ITensor* TensorRTEngine::DeclareInput(const std::string& name,
                                                nvinfer1::DataType dtype,
                                                const nvinfer1::Dims& dim) {
  PADDLE_ENFORCE_EQ(0, buffer_sizes_.count(name), "duplicate input name %s",
                    name);

  PADDLE_ENFORCE(infer_network_ != nullptr, "should initnetwork first");
  auto* input = infer_network_->addInput(name.c_str(), dtype, dim);
  PADDLE_ENFORCE(input, "infer network add input %s failed", name);
  buffer_sizes_[name] = kDataTypeSize[static_cast<int>(dtype)] * AccumDims(dim);
  TensorRTEngine::SetITensor(name, input);
  return input;
}

nvinfer1::ITensor* TensorRTEngine::DeclareInput(int offset) {
  // This is a trick to reuse some facility of the manual network building.
  auto name = ibuffer_name(offset);
  PADDLE_ENFORCE_EQ(0, buffer_sizes_.count(name), "duplicate input name %s",
                    name);
  PADDLE_ENFORCE(infer_network_ != nullptr, "should initnetwork first");
  auto* x = infer_network_->getInput(offset);
  x->setName(name);
  buffer_sizes_[name] = kDataTypeSize[static_cast<int>(x->getType())] *
                        AccumDims(x->getDimensions());
  return x;
}

void TensorRTEngine::DeclareOutput(const nvinfer1::ILayer* layer, int offset,
                                   const std::string& name) {
  PADDLE_ENFORCE_EQ(0, buffer_sizes_.count(name), "duplicate output name %s",
                    name);

  auto* output = layer->getOutput(offset);
  PADDLE_ENFORCE(output != nullptr);
  output->setName(name.c_str());
  infer_network_->markOutput(*output);
  // output buffers' size can only be decided latter, set zero here to mark this
  // and will reset latter.
  buffer_sizes_[name] = 0;
}

void TensorRTEngine::DeclareOutput(const std::string& name) {
  PADDLE_ENFORCE_EQ(0, buffer_sizes_.count(name), "duplicate output name %s",
                    name);

  auto* output = TensorRTEngine::GetITensor(name);
  PADDLE_ENFORCE(output != nullptr);
  output->setName(name.c_str());
  infer_network_->markOutput(*output);
  // output buffers' size can only be decided latter, set zero here to mark this
  // and will reset latter.
  buffer_sizes_[name] = 0;
}

void TensorRTEngine::DeclareOutput(int offset) {
  // This is a trick to reuse some facility of the manual network building.
  auto name = obuffer_name(offset);
  PADDLE_ENFORCE_EQ(0, buffer_sizes_.count(name), "duplicate input name %s",
                    name);
  PADDLE_ENFORCE(infer_network_ != nullptr, "should initnetwork first");
  auto* x = infer_network_->getInput(offset);
  x->setName(name.c_str());
  buffer_sizes_[name] = kDataTypeSize[static_cast<int>(x->getType())] *
      AccumDims(x->getDimensions());
}

void* TensorRTEngine::GetOutputInGPU(const std::string& name) {
  return buffer(name).buffer;
}

void TensorRTEngine::GetOutputInCPU(const std::string& name, void* dst,
                                    size_t max_size) {
  // determine data size
  auto it = buffer_sizes_.find(name);
  PADDLE_ENFORCE(it != buffer_sizes_.end());
  PADDLE_ENFORCE_GT(it->second, 0);
  PADDLE_ENFORCE_GE(max_size, it->second);
  auto& buf = buffer(name);
  PADDLE_ENFORCE_NOT_NULL(buf.buffer, "buffer should be allocated before");
  PADDLE_ENFORCE_EQ(0, cudaMemcpyAsync(dst, buf.buffer, it->second,
                                       cudaMemcpyDeviceToHost, *stream_));
}

Buffer& TensorRTEngine::buffer(const std::string& name) {
  PADDLE_ENFORCE(infer_engine_ != nullptr, "call FreezeNetwork first.");
  auto it = buffer_sizes_.find(name);
  PADDLE_ENFORCE(it != buffer_sizes_.end());
  auto slot_offset = infer_engine_->getBindingIndex(name.c_str());
  return buffers_[slot_offset];
}

Buffer &TensorRTEngine::ibuffer(int offset) {
  auto name = ibuffer_name(offset);
  PADDLE_ENFORCE(infer_engine_ != nullptr, "call FreezeNetwork first.");
  auto it = buffer_sizes_.find(name);
  PADDLE_ENFORCE(it != buffer_sizes_.end());
  auto slot_offset = infer_engine_->getBindingIndex(name.c_str());
  return buffers_[slot_offset];
}

void TensorRTEngine::SetInputFromCPU(const std::string& name, void* data,
                                     size_t size) {
  auto& buf = buffer(name);
  PADDLE_ENFORCE_NOT_NULL(buf.buffer);
  PADDLE_ENFORCE_LE(size, buf.max_size, "buffer is too small");
  PADDLE_ENFORCE(buf.device == DeviceType::GPU);
  PADDLE_ENFORCE_EQ(0, cudaMemcpyAsync(buf.buffer, data, size,
                                       cudaMemcpyHostToDevice, *stream_));
}

void TensorRTEngine::SetITensor(const std::string& name,
                                nvinfer1::ITensor* tensor) {
  PADDLE_ENFORCE(tensor != nullptr);
  PADDLE_ENFORCE_EQ(0, itensor_map_.count(name), "duplicate itensor name %s",
                    name);
  itensor_map_[name] = tensor;
}

nvinfer1::ITensor* TensorRTEngine::GetITensor(const std::string& name) {
  PADDLE_ENFORCE(itensor_map_.count(name), "no itensor %s", name);
  return itensor_map_[name];
}

}  // namespace tensorrt
}  // namespace inference
}  // namespace paddle
