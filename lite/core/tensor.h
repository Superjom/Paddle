// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <algorithm>
#include <functional>  // for multiplies
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "lite/core/memory.h"

namespace paddle {
namespace lite {

class DDimLite;
class TensorLite;

using DDim = lite::DDimLite;
using Tensor = lite::TensorLite;

class DDimLite {
 public:
  using value_type = int64_t;

  DDimLite() = default;

  explicit DDimLite(const std::vector<value_type> &x) { ConstructFrom(x); }

  void ConstructFrom(const std::vector<value_type> &x) { data_ = x; }

  value_type operator[](int offset) const { return data_[offset]; }
  value_type &operator[](int offset) { return data_[offset]; }
  std::vector<int64_t> Vectorize() const { return data_; }

  size_t size() const { return data_.size(); }
  bool empty() const { return data_.empty(); }

  value_type production() const;

  const std::vector<value_type> &data() const { return data_; }
  value_type count(int start, int end) const;

  DDimLite Slice(int start, int end) const;

  DDimLite Flattern2D(int col) const;

  std::string repr() const;

  friend std::ostream &operator<<(std::ostream &os, const DDimLite &dims);

  friend bool operator==(const DDimLite &a, const DDimLite &b);

  friend bool operator!=(const DDimLite &a, const DDimLite &b);

 private:
  std::vector<value_type> data_;
};

using LoD = std::vector<std::vector<uint64_t>>;

// A light-weight tensor implementation.
class TensorLite {
 public:
  TensorLite() : buffer_(std::make_shared<Buffer>()) {}

  template <typename DType, typename DimT, TargetType Target>
  void Assign(DType *data, const DimT &dim) {
    Resize(dim);
    auto *dst = mutable_data<DType>(Target);
    CopySync<Target>(
        dst, data, dim.production() * sizeof(DType), IoDirection::HtoD);
  }

  // T is the data type and R is the return type
  // For OpenCL, the return type can be cl::Buffer
  // and the data type can be float/int8_t.
  // For other devices, T and R may be the same type.
  template <typename T, typename R = T>
  const R *data() const {
    return static_cast<const R *>(buffer_->data());
  }

  void Resize(const DDimLite &ddim) { dims_ = ddim; }
  void Resize(const std::vector<int64_t> &x) { dims_ = DDimLite(x); }

  const DDimLite &dims() const { return dims_; }
  int64_t numel() const { return dims_.production(); }

  const LoD &lod() const { return lod_; }
  LoD *mutable_lod() { return &lod_; }

  // T is the data type and R is the return type
  // For OpenCL, the return type can be cl::Buffer
  // and the data type can be float/int8_t.
  // For other devices, T and R may be the same type.
  template <typename T, typename R = T>
  R *mutable_data();

  // T is the data type and R is the return type
  // For OpenCL, the return type can be cl::Buffer
  // and the data type can be float/int8_t.
  // For other devices, T and R may be the same type.
  template <typename T, typename R = T>
  R *mutable_data(TargetType target);
  void *mutable_data(size_t memory_size);
  void *mutable_data(TargetType target, size_t memory_size);

  const void *raw_data() const { return buffer_->data(); }

  size_t data_size() const { return this->dims().production(); }

  size_t memory_size() const { return memory_size_; }

  bool IsInitialized() const { return buffer_->data(); }

  // Other share data to this.
  void ShareDataWith(const TensorLite &other);

  void CopyDataFrom(const TensorLite &other);

  TargetType target() const { return target_; }

  friend std::ostream &operator<<(std::ostream &os, const TensorLite &tensor);

 private:
  TargetType target_{TargetType::kHost};
  DDimLite dims_;
  std::shared_ptr<Buffer> buffer_;
  LoD lod_;
  size_t memory_size_{};
};

template <typename T, typename R>
R *TensorLite::mutable_data() {
  memory_size_ = dims_.production() * sizeof(T);
  buffer_->ResetLazy(target_, memory_size_);
  return static_cast<R *>(buffer_->data());
}

template <typename T, typename R>
R *TensorLite::mutable_data(TargetType target) {
  target_ = target;
  memory_size_ = dims_.production() * sizeof(T);
  buffer_->ResetLazy(target, memory_size());
  return static_cast<R *>(buffer_->data());
}

template <typename TensorT>
bool TensorCompareWith(const TensorT &a, const TensorT &b) {
  if (a.dims() != b.dims()) return false;
  if (memcmp(a.raw_data(), b.raw_data(), a.data_size()) != 0) return false;
  return true;
}

}  // namespace lite
}  // namespace paddle
