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

#include "lite/operators/io_copy_once_op.h"
#include "lite/core/op_registry.h"

namespace paddle {
namespace lite {
namespace operators {

bool IoCopyOnceOp::run_once() const { return true; }

std::string IoCopyOnceOp::DebugString() const { return "io_copy_once_op"; }

}  // namespace operators
}  // namespace lite
}  // namespace paddle

REGISTER_LITE_OP(io_copy_once, paddle::lite::operators::IoCopyOnceOp);
