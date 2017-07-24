/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#pragma once

#include "paddle/framework/operator.h"

namespace paddle {
namespace operators {

using namespace paddle::framework;

namespace rnn {

/*
 * Memory of a RNN (same as the role of `Momory` in PaddlePaddle).
 *
 * Memory attributes cached by this op, dims will be infered from
 * boot memories in father scope. Other attributes are copied from Op's proto
 * attributes.
 */
struct MemoryAttr {
  // name of current state variable
  std::string var;
  // name of previous step's state variable
  std::string pre_var;
  // name of the variables to init this memory (same role of `boot_layer` in
  // PaddlePaddle), which is store in father's scope.
  std::string boot_var;
};

struct Link {
  // input or output links name.
  std::string internal;
  // alias to avoid duplicate keys in scopes.
  std::string external;
};

struct Argument {
  std::string step_net;
  std::string step_scopes;
  std::vector<Link> inlinks;
  std::vector<Link> outlinks;
  std::vector<rnn::MemoryAttr> memories;
};

struct ArgumentName {
  std::string step_net;
  std::string step_scopes;
  std::string inlinks;
  std::string outlinks;
  std::string inlink_alias;   // the alias of inlinks in step net.
  std::string outlink_alias;  // the alias of outlinks in step net.
  std::string memories;       // the memory name
  std::string pre_memories;   // the previous memory name
  std::string boot_memories;  // the boot memory name
};

/*
 * Prepare inputs for each stepnet.
 */
void SegmentInputs(std::vector<ScopePtr>& step_scopes,
                   const std::vector<Link>& inlinks,
                   const size_t seq_len);

/*
 * Process outputs of stepnets and merge to variables.
 */
void ConcatOutputs(std::vector<ScopePtr>& step_scopes,
                   const std::vector<Link>& outlinks,
                   const size_t seq_len);

void LinkMemories(std::vector<ScopePtr>& step_scopes,
                  const std::vector<MemoryAttr>& memories,
                  size_t step_id,
                  int offset);

void InitArgument(const ArgumentName& name, Argument* arg);

};  // namespace rnn

// The sequence format in RecurrentOp is Tensor<seq_len, batch_size, dim> now.
// TODO:
// 1. No-padding computing for sequences with indifinite length in one batch.
// 2. Hierarchical RNN for sequence with sub-sequence.
// 3. Internal Memory.
// 4. More Complex RNN architecture, such as Gated Feedback RNN.
//    Refer to: https://arxiv.org/pdf/1502.02367.pdf

class RecurrentAlgorithm {
public:
  /*
   * Forward run the RNN.
   *
   * NOTE the context's scope is not given until `Run` called, so step scopes'
   * father should be set/updated in this method.
   */
  void Run(const ScopePtr& scope, const platform::DeviceContext& dev_ctx) const;

  void Init(std::unique_ptr<rnn::Argument> arg) { arg_ = std::move(arg); }

  void InferShape(const ScopePtr& scope) const;

  std::string debug_string() const;

protected:
  /*
   * the step scopes as the father scope. The step scopes will be stored in
   * the father scope as a variable whose name is specified by
   * `step_scopes_name_`.
   *
   * NOTE the scopes are reused by both the `Forward` and `Backward`, so just
   * create once and expand its size if more steps need.
   */
  void CreateScopes(ScopePtr scope) const;

  /*
   * Get the step scopes.
   */
  inline const std::vector<ScopePtr>& GetStepScopes(ScopePtr scope) const {
    return *(scope->GetVariable(arg_->step_scopes))
                ->GetMutable<std::vector<ScopePtr>>();
  }

  /*
   * Init memories.
   */
  void InitMemories(ScopePtr step_scopes) const;

private:
  std::unique_ptr<rnn::Argument> arg_;
  mutable size_t seq_len_;
};

/*
 * RNN's backward alogorithm.
 *
 * To accelerate the development of RecurrentGradientOp, we decouple RNN's
 * algorithm and `OperatorBase`'s implementation, the former contains the core
 * implementation of a RNN, and will keep stable even if the framework changes a
 * lot, and the latter is a wrapper acts like an dapter for it to make RNN an
 * operator.
 */
class RecurrentGradientAlgorithm {
public:
  void Init(std::unique_ptr<rnn::Argument> arg) { arg_ = std::move(arg); }
  void Run(const ScopePtr& scope, const platform::DeviceContext& dev_ctx) const;
  void LinkBootMemoryGradients(ScopePtr step_scopes) const;
  void InferShape(const ScopePtr& scope) const;

private:
  std::unique_ptr<rnn::Argument> arg_;
  mutable size_t seq_len_;
};

/*
 * RNN forward's op wrapper.
 */
class RecurrentOp final : public OperatorBase {
public:
  void Init() override;

  virtual void InferShape(const ScopePtr& scope) const {
    alg_.InferShape(scope);
  }

  virtual void Run(const ScopePtr& scope,
                   const platform::DeviceContext& dev_ctx) const override {}

  virtual ~RecurrentOp() {}

  static const rnn::ArgumentName arg_name;

private:
  RecurrentAlgorithm alg_;
};

/*
 * RNN backward's op wrapper.
 */
class RecurrentGradientOp final : public OperatorBase {
public:
  void Init() override;

  virtual void InferShape(const ScopePtr& scope) const {
    alg_.InferShape(scope);
  }

  virtual void Run(const ScopePtr& scope,
                   const platform::DeviceContext& dev_ctx) const override {}

  virtual ~RecurrentGradientOp() {}

  static const rnn::ArgumentName arg_name;

private:
  RecurrentGradientAlgorithm alg_;
};

}  // namespace operators
}  // namespace paddle
