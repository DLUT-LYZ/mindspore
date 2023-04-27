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

#include "runtime/device/gsm/swap_manager.h"

#include <functional>
#include <string>
#include <utility>

#include "include/common/utils/offload_context.h"
#include "utils/file_utils.h"

namespace mindspore {
namespace device {
constexpr char kLinuxAioLibName[] = "libaio_plugin.so";
constexpr char kLinuxAioInstanceFuncName[] = "get_aio_instance";
constexpr size_t kFirstSizeLevel = 0xFFFFFFFFFFFFFFFF << 24;  // 16M
SwapManager::SwapManager(size_t stream_id, mindspore::device::DynamicMemPoolBestFit *device_memory_pool,
                         PinMemPool *pin_mem_pool)
    : stream_id_(stream_id), device_memory_pool_(device_memory_pool), pin_mem_pool_(pin_mem_pool) {
  const auto &offload_context = OffloadContext::GetInstance();
  io_handle_ = std::make_shared<IOHandle>();
  if (offload_context != nullptr) {
    if (offload_context->enable_aio()) {
      io_handle_->LoadAio(kLinuxAioLibName, kLinuxAioInstanceFuncName);
    }
    max_file_size_ = offload_context->offload_disk_size();
  }
  swappable_tensors_.resize(size_level_num_ + 1);
}

template <class Input, class Output>
bool SwapManager::TryAllocate(std::queue<const DeviceAddress *> queue, const Input &input,
                              Output (SwapManager::*allocate_func)(const Input &),
                              const std::function<bool(Output)> &success, Output *output) {
  MS_EXCEPTION_IF_NULL(allocate_func);
  MS_EXCEPTION_IF_NULL(output);
  (*output) = (this->*allocate_func)(input);
  if (success(*output)) {
    return true;
  }
  // Wait swapping tensors.
  while (!queue.empty()) {
    const auto &front = queue.front();
    MS_EXCEPTION_IF_NULL(front);
    if (front->Wait()) {
      (*output) = (this->*allocate_func)(input);
      if (success(*output)) {
        return true;
      }
    }
    queue.pop();
  }
  return false;
}

template <class Input, class Output>
bool SwapManager::SwapOutTemp(const std::pair<DeviceAddressStatus, StorageType> &swap_type, size_t total_size,
                              const Input &input,
                              Output (mindspore::device::SwapManager::*allocate_func)(const Input &),
                              const std::function<bool(Output)> &success, Output *output) {
  MS_EXCEPTION_IF_NULL(allocate_func);
  MS_EXCEPTION_IF_NULL(output);
  const auto target_device_address_status = swap_type.first;
  const auto swap_out_to = swap_type.second;
  const auto size_level = GetSizeLevel(total_size);
  const auto swap_temp_func = [&](const DeviceAddressPtr &candidate) -> bool {
    if (!candidate->swappable() || candidate->status() != target_device_address_status) {
      return false;
    }
    if (candidate->status() == DeviceAddressStatus::kInDevice && candidate->GetPtr() == nullptr) {
      return false;
    }
    candidate->MoveTo(swap_out_to, false, kDefaultStreamIndex);
    (*output) = (this->*allocate_func)(input);
    return success(*output);
  };
  for (size_t sl = size_level; sl <= size_level_num_; ++sl) {
    const auto &candidates = swappable_tensors_[sl];
    for (const auto &swappable_tensor : candidates) {
      if (swappable_tensor->GetSize() < total_size) {
        continue;
      }
      if (swap_temp_func(swappable_tensor)) {
        return true;
      }
    }
  }
  for (auto riter = swappable_tensors_.crbegin(); riter != swappable_tensors_.crend(); ++riter) {
    const auto &candidates = *riter;
    for (const auto &swappable_tensor : candidates) {
      if (swap_temp_func(swappable_tensor)) {
        return true;
      }
    }
  }
  return false;
}

void *SwapManager::AllocDeviceMemorySimply(const size_t &size) {
  MS_EXCEPTION_IF_NULL(device_memory_pool_);
  return device_memory_pool_->AllocTensorMem(size);
}

void *SwapManager::AllocDeviceMemory(size_t size) {
  void *ret = nullptr;
  void *(SwapManager::*allocate_func)(const size_t &) = &SwapManager::AllocDeviceMemorySimply;
  std::function<bool(void *)> success = [](void *ptr) { return ptr != nullptr; };
  std::lock_guard<std::mutex> lock(swapping_tensors_device_mutex_);
  if (!TryAllocate(swapping_tensors_device_, size, allocate_func, success, &ret) &&
      !SwapOutTemp(std::make_pair(DeviceAddressStatus::kInDevice, StorageType::kHost), size, size, allocate_func,
                   success, &ret)) {
    MS_LOG(WARNING) << "Allocate device memory failed, size: " << size;
  }
  return ret;
}

std::vector<void *> SwapManager::AllocDeviceContinuousMemSimply(const std::vector<size_t> &size_list) {
  MS_EXCEPTION_IF_NULL(device_memory_pool_);
  return device_memory_pool_->AllocContinuousTensorMem(size_list);
}

std::vector<void *> SwapManager::AllocDeviceContinuousMem(const std::vector<size_t> &size_list) {
  std::vector<void *> ret;
  std::vector<void *> (SwapManager::*allocate_func)(const std::vector<size_t> &) =
    &SwapManager::AllocDeviceContinuousMemSimply;
  std::function<bool(std::vector<void *>)> success = [](const std::vector<void *> &ptrs) { return !ptrs.empty(); };
  std::lock_guard<std::mutex> lock(swapping_tensors_device_mutex_);
  if (!TryAllocate(swapping_tensors_device_, size_list, allocate_func, success, &ret)) {
    const size_t total_size = std::accumulate(size_list.begin(), size_list.end(), size_t(1), std::multiplies<>());
    if (!SwapOutTemp(std::make_pair(DeviceAddressStatus::kInDevice, StorageType::kHost), total_size, size_list,
                     allocate_func, success, &ret)) {
      MS_LOG(WARNING) << "Allocate continuous device mem failed, size list: " << size_list;
    }
  }
  return ret;
}

void SwapManager::FreeDeviceMemory(void *ptr) {
  MS_EXCEPTION_IF_NULL(device_memory_pool_);
  device_memory_pool_->FreeTensorMem(ptr);
}

void *SwapManager::AllocHostMemorySimply(const size_t &size) {
  MS_EXCEPTION_IF_NULL(pin_mem_pool_);
  return pin_mem_pool_->AllocPinMem(size);
}

void *SwapManager::AllocHostMemory(size_t size) {
  void *ret = nullptr;
  void *(SwapManager::*allocate_func)(const size_t &) = &SwapManager::AllocHostMemorySimply;
  std::function<bool(void *)> success = [](void *ptr) { return ptr != nullptr; };
  std::lock_guard<std::mutex> lock(swapping_tensors_host_mutex_);
  if (!TryAllocate(swapping_tensors_host_, size, allocate_func, success, &ret) &&
      !SwapOutTemp(std::make_pair(DeviceAddressStatus::kInHost, StorageType::kFile), size, size, allocate_func, success,
                   &ret)) {
    MS_LOG(WARNING) << "Allocate host memory failed, size: " << size;
  }
  return ret;
}

void SwapManager::FreeHostMemory(void *ptr) {
  MS_EXCEPTION_IF_NULL(pin_mem_pool_);
  pin_mem_pool_->FreeTensorMem(ptr);
}

bool SwapManager::CreateFile(const std::string &file_name, size_t file_size) {
  MS_EXCEPTION_IF_NULL(io_handle_);
  bool (SwapManager::*allocate_func)(const size_t &size) = &SwapManager::EnoughFileSpace;
  std::function<bool(bool)> success = [](bool ret) { return ret; };
  {
    std::lock_guard<std::mutex> lock(swapping_tensors_file_mutex_);
    bool enough = false;
    if (!TryAllocate(swapping_tensors_file_, file_size, allocate_func, success, &enough)) {
      MS_LOG(WARNING) << "There is no enough disk space for creating file, size: " << file_size;
      return false;
    }
  }
  current_used_file_size_ += file_size;
  file_size_[file_name] = file_size;
  return io_handle_->CreateSwapFile(file_name);
}

bool SwapManager::DeleteFile(const std::string &file_name) {
  MS_EXCEPTION_IF_NULL(io_handle_);
  const auto &iter = file_size_.find(file_name);
  if (iter == file_size_.end()) {
    MS_LOG(WARNING) << "Can not file size for file[" << file_name << "]";
  } else {
    current_used_file_size_ -= iter->second;
    iter->second = 0;
  }
  return io_handle_->DeleteSwapFile(file_name);
}

bool SwapManager::FileToHostMemory(void *host_memory, const std::string &file_name, size_t byte_num, bool async,
                                   AsyncIOToken *sync_key) {
  MS_EXCEPTION_IF_NULL(io_handle_);
  if (async) {
    return io_handle_->ReadAsync(file_name, host_memory, byte_num, sync_key);
  } else {
    return io_handle_->Read(file_name, host_memory, byte_num);
  }
}

bool SwapManager::EnoughFileSpace(const size_t &size) { return current_used_file_size_ + size <= max_file_size_; }

bool SwapManager::HostMemoryToFile(const std::string &file_name, const void *data, size_t byte_num, bool async,
                                   AsyncIOToken *sync_key) {
  MS_EXCEPTION_IF_NULL(io_handle_);
  if (async) {
    return io_handle_->WriteAsync(file_name, data, byte_num, sync_key);
  } else {
    return io_handle_->Write(file_name, data, byte_num);
  }
}

bool SwapManager::WaitAsyncIO(mindspore::device::AsyncIOToken sync_token) {
  MS_EXCEPTION_IF_NULL(io_handle_);
  return io_handle_->Wait(sync_token);
}

size_t SwapManager::GetSizeLevel(size_t size) const {
  size_t mask = kFirstSizeLevel;
  for (size_t i = 0; i < size_level_num_; i += 1) {
    if ((size & mask) == 0) {
      return i;
    }
    mask = mask << 1;
  }
  return size_level_num_;
}

void SwapManager::AddSwappableTensor(const DeviceAddressPtr &device_address) {
  if (all_swappable_tensors_.count(device_address) != 0) {
    return;
  }
  size_t size_level = GetSizeLevel(device_address->GetSize());
  swappable_tensors_[size_level].emplace_back(device_address);
  all_swappable_tensors_.insert(device_address);
}

void SwapManager::AddSwappingTensor(const mindspore::device::DeviceAddress *device_address) {
  if (device_address == nullptr) {
    return;
  }
  if (device_address->status() == DeviceAddressStatus::kInFileToHost) {
    std::lock_guard<std::mutex> lock(swapping_tensors_file_mutex_);
    (void)swapping_tensors_file_.push(device_address);
  } else if (device_address->status() == DeviceAddressStatus::kInDeviceToHost) {
    std::lock_guard<std::mutex> lock(swapping_tensors_device_mutex_);
    (void)swapping_tensors_device_.push(device_address);
  } else {
    std::lock_guard<std::mutex> lock(swapping_tensors_host_mutex_);
    (void)swapping_tensors_host_.push(device_address);
  }
}

void SwapManager::SetSwappableBeforeMemAllocate(const std::vector<DeviceAddress *> &inputs,
                                                const std::vector<DeviceAddress *> &outputs) {
  for (const auto &device_address : inputs) {
    MS_EXCEPTION_IF_NULL(device_address);
    device_address->set_swappable(false);
  }
  for (const auto &device_address : outputs) {
    MS_EXCEPTION_IF_NULL(device_address);
    device_address->set_swappable(false);
  }
}

void SwapManager::SetSwappableBeforeMemFree(const std::vector<DeviceAddress *> &inputs,
                                            const std::vector<DeviceAddress *> &outputs,
                                            const mindspore::device::KernelInfo *kernel_info) {
  for (const auto &device_address : inputs) {
    MS_EXCEPTION_IF_NULL(device_address);
    device_address->set_swappable(true);
  }
  for (const auto &device_address : outputs) {
    MS_EXCEPTION_IF_NULL(device_address);
    device_address->set_swappable(true);
  }
  for (const auto &out_in : kernel_info->out_in_ref_map()) {
    if (inputs[out_in.second] != outputs[out_in.first]) {
      outputs[out_in.first]->set_swappable(false);
    }
  }
}
}  // namespace device
}  // namespace mindspore
