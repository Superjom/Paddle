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

#include <vector>
#include "paddle/framework/executor.h"
#include "paddle/framework/op_registry.h"

namespace paddle {
namespace operators {
constexpr char kInputs[] = "inputs";
constexpr char kInitialStates[] = "initial_states";
constexpr char kParameters[] = "parameters";
constexpr char kOutputs[] = "outputs";
constexpr char kStepScopes[] = "step_scopes";
constexpr char kExStates[] = "ex_states";
constexpr char kStates[] = "states";
constexpr char kStepBlock[] = "step_block";
constexpr char kReverse[] = "reverse";
constexpr char kIsTrain[] = "is_train";
#define GRAD_SUFFIX "@GRAD"
constexpr char kInputGrads[] = "inputs" GRAD_SUFFIX;
constexpr char kOutputGrads[] = "outputs" GRAD_SUFFIX;
constexpr char kParamGrads[] = "parameters" GRAD_SUFFIX;
constexpr char kInitStateGrads[] = "initial_states" GRAD_SUFFIX;

using StepScopeVar = std::vector<framework::Scope *>;

// StepScopes manages scopes inside RNN.
//    StepScopes::CurScope() get the current scope
//    StepScopes::ExScope() get the ex-scope, or scope in previous time step.
//    StepScopes::Next() move to next time step.
//
// if is_train = False, then
//   there are two scopes for the RNN and just support forward.
// else
//   the len(scopes) == seq_len
//
// if is_backward = True, then
//   reversely access scopes
// else
//   access scopes from begin to end.
class StepScopes {
 public:
  StepScopes(const framework::Scope &parent, StepScopeVar *scopes,
             bool is_train, size_t seq_len, bool is_backward = false)
      : counter_(is_backward ? seq_len - 1 : 0UL),
        scopes_(scopes),
        is_train_(is_train),
        is_backward_(is_backward) {
    size_t num_step_scopes = is_train ? seq_len : 2;
    PADDLE_ENFORCE(!is_train && is_backward,
                   "Cannot backward when is not training");
    if (!is_backward_) {
      PADDLE_ENFORCE(scopes->empty());
      scopes->reserve(static_cast<size_t>(num_step_scopes));
      for (size_t i = 0; i < num_step_scopes; ++i) {
        scopes->emplace_back(&parent.NewScope());
      }
    }
  }

  framework::Scope &CurScope() { return GetScope(counter_); }

  framework::Scope &ExScope() {
    auto &scope = GetScope(is_backward_ ? counter_ + 1 : counter_ - 1);
    return scope;
  }

  void Next() {
    if (is_backward_) {
      --counter_;
    } else {
      ++counter_;
    }
  }

 private:
  framework::Scope &GetScope(size_t scope_id) const {
    if (!is_train_) {
      scope_id %= 2;
    }
    PADDLE_ENFORCE_LT(scope_id, scopes_->size());
    return *(*scopes_)[scope_id];
  }

  size_t counter_;
  StepScopeVar *scopes_;
  bool is_train_;
  bool is_backward_;
};

// Base class for RecurrentOp/RecurrentGradOp
//    Some common protected functions for RecurrentOp/RecurrentGradOp
class RecurrentBase : public framework::OperatorBase {
 public:
  RecurrentBase(const std::string &type,
                const framework::VariableNameMap &inputs,
                const framework::VariableNameMap &outputs,
                const framework::AttributeMap &attrs)
      : OperatorBase(type, inputs, outputs, attrs) {}

 protected:
  // Get SequenceLength from Scope
  //   The sequence length is got from input tensor. The input tensor's
  //   dimension should be [SEQ_LEN, ..., ...]. The first of the tensor's shape
  //   is SEQ_LEN. The second of the tensor's shape could be the batch size or
  //   nested sequence length.
  int64_t GetSequenceLength(const framework::Scope &scope) const {
    // Dim format SEQ_LEN, BATCH_SIZE, ...
    int64_t seq_len = -1;
    auto &all_inputs = Inputs(kInputs);
    PADDLE_ENFORCE(!all_inputs.empty());
    for (auto &iname : all_inputs) {
      auto *var = scope.FindVar(iname);
      PADDLE_ENFORCE(var != nullptr);
      PADDLE_ENFORCE(var->IsType<framework::LoDTensor>());
      auto &dim = var->Get<framework::LoDTensor>().dims();
      if (seq_len == -1) {
        seq_len = dim[0];
      } else {
        PADDLE_ENFORCE_EQ(seq_len, dim[0]);
      }
    }
    return seq_len;
  }

  // for src_tensor, dst_tensor in zip(map(src_scope.FindVar, src_vars),
  //                                   map(dst_scope.Var, dst_vars)):
  //   dst_tensor.ShareDataWith(src_tensor)
  static void LinkTensor(const framework::Scope &src_scope,
                         const std::vector<std::string> &src_vars,
                         framework::Scope *dst_scope,
                         const std::vector<std::string> &dst_vars) {
    LinkTensorWithCallback(
        src_scope, src_vars, dst_scope, dst_vars,
        [&](const framework::Tensor &src, framework::Tensor *dst) {
          dst->ShareDataWith(src);
        });
  }

  // for src_tensor, dst_tensor in zip(map(src_scope.FindVar, src_vars),
  //                                   map(dst_scope.Var, dst_vars)):
  //   callback(src_tensor, &dst_tensor)
  template <typename Callback>
  static void LinkTensorWithCallback(const framework::Scope &src_scope,
                                     const std::vector<std::string> &src_vars,
                                     framework::Scope *dst_scope,
                                     const std::vector<std::string> &dst_vars,
                                     Callback callback) {
    PADDLE_ENFORCE_EQ(src_vars.size(), dst_vars.size());
    for (size_t i = 0; i < dst_vars.size(); ++i) {
      AccessTensor(src_scope, src_vars[i], dst_scope, dst_vars[i], callback);
    }
  }

  // for src_tensor, dst_tensor in zip(map(src_scope.FindVar, src_vars),
  //                                   map(dst_scope.FindVar, dst_vars)):
  //   callback(src_tensor, &dst_tensor)
  template <typename Callback>
  static void LinkTensorWithCallback(const framework::Scope &src_scope,
                                     const std::vector<std::string> &src_vars,
                                     const framework::Scope &dst_scope,
                                     const std::vector<std::string> &dst_vars,
                                     Callback callback) {
    PADDLE_ENFORCE_EQ(src_vars.size(), dst_vars.size());
    for (size_t i = 0; i < dst_vars.size(); ++i) {
      AccessTensor(src_scope, src_vars[i], dst_scope, dst_vars[i], callback);
    }
  }

  // (seq_len, shape) -> return [seq_len] + list(shape)
  static framework::DDim PrependDims(size_t seq_len,
                                     const framework::DDim &src) {
    auto dims = framework::vectorize(src);
    dims.insert(dims.begin(), static_cast<int64_t>(seq_len));
    return framework::make_ddim(dims);
  }

 private:
  template <typename Callback>
  static void AccessTensor(const framework::Scope &src_scope,
                           const std::string &src_var_name,
                           framework::Scope *dst_scope,
                           const std::string &dst_var_name, Callback callback) {
    auto *src_var = src_scope.FindVar(src_var_name);
    PADDLE_ENFORCE(src_var != nullptr);
    auto &src_tensor = src_var->Get<framework::LoDTensor>();

    auto *dst_var = dst_scope->Var(dst_var_name);
    auto *dst_tensor = dst_var->GetMutable<framework::LoDTensor>();
    callback(src_tensor, dst_tensor);
  }

  template <typename Callback>
  static void AccessTensor(const framework::Scope &src_scope,
                           const std::string &src_var_name,
                           const framework::Scope &dst_scope,
                           const std::string &dst_var_name, Callback callback) {
    auto *src_var = src_scope.FindVar(src_var_name);
    PADDLE_ENFORCE(src_var != nullptr);
    auto &src_tensor = src_var->Get<framework::LoDTensor>();
    auto *dst_var = dst_scope.FindVar(dst_var_name);
    PADDLE_ENFORCE(dst_var != nullptr);
    auto *dst_tensor = dst_var->GetMutable<framework::LoDTensor>();
    callback(src_tensor, dst_tensor);
  }
};

class RecurrentOp : public RecurrentBase {
 public:
  RecurrentOp(const std::string &type, const framework::VariableNameMap &inputs,
              const framework::VariableNameMap &outputs,
              const framework::AttributeMap &attrs)
      : RecurrentBase(type, inputs, outputs, attrs) {}

  void Run(const framework::Scope &scope,
           const platform::DeviceContext &dev_ctx) const override {
    auto seq_len = static_cast<size_t>(this->GetSequenceLength(scope));
    VLOG(3) << "Static RNN input sequence length = " << seq_len;
    StepScopes scopes = CreateStepScopes(scope, seq_len);
    auto reverse = Attr<bool>(kReverse);

    framework::Executor executor(dev_ctx);
    auto *block = Attr<framework::BlockDescBind *>(kStepBlock);
    auto *program = block->Program();

    for (size_t i = 0; i < seq_len; ++i) {
      size_t seq_offset = reverse ? seq_len - i - 1 : i;
      VLOG(3) << "Recurrent operate at the time step " << seq_offset;

      auto &cur_scope = scopes.CurScope();

      // Link outside::input --> inside::input
      //   inside::input = outside::input[seq_offset: seq_offset+1]
      LinkTensorWithCallback(
          scope, Inputs(kInputs), &cur_scope, Inputs(kInputs),
          [&seq_offset](const framework::Tensor &outside,
                        framework::Tensor *inside) {
            inside->ShareDataWith(outside.Slice(seq_offset, seq_offset + 1));
            auto dims = framework::vectorize(inside->dims());
            dims.erase(dims.begin());
            inside->Resize(framework::make_ddim(dims));
          });

      if (i == 0) {
        // Link initial states  --> ex_states
        LinkTensor(scope, Inputs(kInitialStates), &cur_scope,
                   Attr<std::vector<std::string>>(kExStates));
      } else {
        auto &ex_scope = scopes.ExScope();
        // Link ex_scope::state --> cur_scope::ex_state
        LinkTensor(ex_scope, Attr<std::vector<std::string>>(kStates),
                   &cur_scope, Attr<std::vector<std::string>>(kExStates));
      }

      // Every inputs are linked now, execute!
      executor.Run(*program, &cur_scope, block->ID(),
                   false /*create_local_scope*/);

      // Copy inside::output -> outside::output
      //    outside::output[seq_offset: seq_offset + 1] = inside::output
      this->LinkTensorWithCallback(
          cur_scope, Outputs(kOutputs), scope, Outputs(kOutputs),
          [&](const framework::LoDTensor &src_tensor,
              framework::LoDTensor *dst_tensor) {
            if (i == 0) {  // create output tensor at begin
              dst_tensor->Resize(PrependDims(seq_len, src_tensor.dims()));
              dst_tensor->mutable_data(dev_ctx.GetPlace(), src_tensor.type());
            }

            auto dst_out = dst_tensor->Slice(seq_offset, seq_offset + 1);
            // Explicit copy output since the local RNN scope can be destroyed
            // early.
            dst_out.CopyFrom(src_tensor, dev_ctx.GetPlace(), dev_ctx);
          });

      scopes.Next();
    }
  }

 private:
  StepScopes CreateStepScopes(const framework::Scope &scope,
                              size_t seq_len) const {
    auto *var = scope.FindVar(Output(kStepScopes));
    PADDLE_ENFORCE(var != nullptr);
    return StepScopes(scope, var->GetMutable<StepScopeVar>(),
                      Attr<bool>(kIsTrain), seq_len);
  }
};

class RecurrentGradOp : public RecurrentBase {
 public:
  RecurrentGradOp(const std::string &type,
                  const framework::VariableNameMap &inputs,
                  const framework::VariableNameMap &outputs,
                  const framework::AttributeMap &attrs)
      : RecurrentBase(type, inputs, outputs, attrs) {}

  void Run(const framework::Scope &scope,
           const platform::DeviceContext &dev_ctx) const override {
    auto seq_len = static_cast<size_t>(GetSequenceLength(scope));
    StepScopes scopes = CreateStepScopes(scope, seq_len);
    auto reverse = Attr<bool>(kReverse);

    framework::Executor executor(dev_ctx);
    auto *block = Attr<framework::BlockDescBind *>(kStepBlock);
    auto *program = block->Program();

    for (size_t step_id = 0; step_id < seq_len; ++step_id) {
      size_t seq_offset = reverse ? step_id : seq_len - step_id - 1;
      VLOG(3) << "Recurrent backward operate at the time step " << seq_offset;
      auto &cur_scope = scopes.CurScope();
      // Link outside::output_grads --> inside::output_grads
      //   inside::output_grad = outside::output_grad[seq_offset:seq_offset+1]
      LinkTensorWithCallback(
          scope, Inputs(kOutputGrads), &cur_scope, Inputs(kOutputGrads),
          [&](const framework::Tensor &outside, framework::Tensor *inside) {
            inside->ShareDataWith(outside.Slice(seq_offset, seq_offset + 1));
            auto dims = framework::vectorize(inside->dims());
            dims.erase(dims.begin());
            inside->Resize(framework::make_ddim(dims));
          });
      auto og_set = List2Set(Inputs(kOutputGrads));

      if (VLOG_IS_ON(10)) {
        std::ostringstream sout;
        std::copy(og_set.begin(), og_set.end(),
                  std::ostream_iterator<std::string>(sout, ","));
        VLOG(10) << " RNN output gradients = [" << sout.str() << "]";
      }

      // Link states
      //   if cur_scope::cur_state_grad in out_grads:
      //     cur_scope::cur_state_grad += ex_scope::ex_state_grad
      //   else:
      //     ex_scope::ex_state_grad --> cur_scope::cur_state_grad
      if (step_id != 0) {  // not at beginning
        auto &ex_scope = scopes.ExScope();
        auto ex_state_grads =
            GradVarLists(Attr<std::vector<std::string>>(kExStates));
        auto cur_state_grads =
            GradVarLists(Attr<std::vector<std::string>>(kStates));

        PADDLE_ENFORCE_EQ(ex_state_grads.size(), cur_state_grads.size());
        for (size_t i = 0; i < ex_state_grads.size(); ++i) {
          auto &cur_grad = cur_state_grads[i];
          auto &ex_grad = ex_state_grads[i];
          auto &ex_tensor =
              ex_scope.FindVar(ex_grad)->Get<framework::LoDTensor>();

          VLOG(10) << " RNN link " << cur_grad << " from " << ex_grad;
          if (og_set.find(cur_grad) != og_set.end()) {
            VLOG(10) << " RNN link " << cur_grad << " with output grad ";
            std::string tmp_var_name;
            auto *tmp_var = cur_scope.Var(&tmp_var_name);
            tmp_var->GetMutable<framework::LoDTensor>()->ShareDataWith(
                ex_tensor);
            auto sum_op = framework::OpRegistry::CreateOp(
                "sum", {{"X", {cur_grad, tmp_var_name}}}, {{"Out", {cur_grad}}},
                {});
            sum_op->Run(cur_scope, dev_ctx);
          } else {
            auto *cur_grad_var = cur_scope.FindVar(cur_grad);
            cur_grad_var->GetMutable<framework::LoDTensor>()->ShareDataWith(
                ex_tensor);
          }
        }
      }

      // Run step block with cur_scope
      executor.Run(*program, &cur_scope, block->ID(),
                   false /*create_local_scope*/);

      auto local_var_names = LocalVarNames(cur_scope);

      // Accumulate params
      //   if (step == 0):
      //      outside::param_grad = 0.0
      //   outside::param_grad += inside::param_grad
      {
        auto &pg_names = Outputs(kParamGrads);
        auto &p_names = Inputs(kParameters);
        PADDLE_ENFORCE_EQ(pg_names.size(), p_names.size());

        for (size_t prog_id = 0; prog_id < pg_names.size(); ++prog_id) {
          auto inside_grad_name = framework::GradVarName(p_names[prog_id]);

          // If does not compute gradient of that variable inside rnn, just
          // continue
          if (local_var_names.find(inside_grad_name) == local_var_names.end()) {
            continue;
          }

          // zero gradient variable in step 0
          if (step_id == 0) {
            auto &inside_tensor = cur_scope.FindVar(inside_grad_name)
                                      ->Get<framework::LoDTensor>();
            framework::AttributeMap attrs;

            attrs["data_type"] = framework::ToDataType(inside_tensor.type());
            attrs["shape"] = framework::vectorize2int(inside_tensor.dims());
            attrs["value"] = 0.0f;

            auto zero_op = framework::OpRegistry::CreateOp(
                "fill_constant", {}, {{"Out", {pg_names[prog_id]}}}, attrs);
            zero_op->Run(scope, dev_ctx);
          }

          // sum gradient
          auto *outside_var = scope.FindVar(pg_names[prog_id]);
          PADDLE_ENFORCE(outside_var != nullptr);
          auto &outside_tensor =
              *outside_var->GetMutable<framework::LoDTensor>();

          std::string result_var_name;
          auto *local_result_var = cur_scope.Var(&result_var_name);
          auto &local_result_tensor =
              *local_result_var->GetMutable<framework::LoDTensor>();

          local_result_tensor.ShareDataWith(outside_tensor);

          auto sum_op = framework::OpRegistry::CreateOp(
              "sum", {{"X", {result_var_name, inside_grad_name}}},
              {{"Out", {result_var_name}}}, {});
          sum_op->Run(cur_scope, dev_ctx);
        }
      }

      // Copy input gradient from inside to outside
      //   outside::input_grad[seq_offset: seq_offset + 1] = inside::input_grad
      LinkTensorWithCallback(
          cur_scope, GradVarLists(Inputs(kInputs)), scope, Outputs(kInputGrads),
          [&](const framework::LoDTensor &inside,
              framework::LoDTensor *outside) {
            if (step_id == 0) {  // alloc memory
              outside->Resize(PrependDims(seq_len, inside.dims()));
              outside->mutable_data(dev_ctx.GetPlace(), inside.type());
            }

            auto dst = outside->Slice(seq_offset, seq_offset + 1);
            dst.CopyFrom(inside, dev_ctx.GetPlace(), dev_ctx);
          });

      if (step_id + 1 == seq_len) {  // at_end
        // copy initialize states gradient from inside to outside
        LinkTensorWithCallback(
            cur_scope, GradVarLists(Attr<std::vector<std::string>>(kExStates)),
            scope, Outputs(kInitStateGrads),
            [&](const framework::LoDTensor &inside,
                framework::LoDTensor *outside) {
              outside->Resize(inside.dims());
              outside->mutable_data(dev_ctx.GetPlace(), inside.type());
              outside->CopyFrom(inside, dev_ctx.GetPlace(), dev_ctx);
            });
      }
      scopes.Next();
    }
  }

 private:
  StepScopes CreateStepScopes(const framework::Scope &scope,
                              size_t seq_len) const {
    auto *var = scope.FindVar(Input(kStepScopes));
    PADDLE_ENFORCE(var != nullptr);
    return StepScopes(scope, var->GetMutable<StepScopeVar>(),
                      Attr<bool>(kIsTrain), seq_len, true /*is_backward*/);
  }

  std::unordered_set<std::string> List2Set(
      const std::vector<std::string> &list) const {
    std::unordered_set<std::string> local_var_name_set;
    local_var_name_set.reserve(list.size());
    for (auto &each : list) {
      local_var_name_set.insert(each);
    }
    return local_var_name_set;
  }

  std::unordered_set<std::string> LocalVarNames(
      const framework::Scope &scope) const {
    return this->List2Set(scope.GetAllNames(false));
  }
  static std::vector<std::string> GradVarLists(
      const std::vector<std::string> &var_names) {
    std::vector<std::string> retv;
    retv.reserve(var_names.size());
    std::transform(var_names.begin(), var_names.end(), std::back_inserter(retv),
                   framework::GradVarName);
    return retv;
  }
};

class RecurrentOpProtoMaker : public framework::OpProtoAndCheckerMaker {
 public:
  RecurrentOpProtoMaker(framework::OpProto *proto,
                        framework::OpAttrChecker *op_checker)
      : OpProtoAndCheckerMaker(proto, op_checker) {
    AddInput(kInputs, "rnn inputs").AsDuplicable();
    AddInput(kInitialStates, "rnn initial states").AsDuplicable();
    AddInput(kParameters,
             "Parameters are used by step block as its input. However, the "
             "inputs is not a sequence tensor. Every time step, each operator "
             "in step block just use the parameter directly")
        .AsDuplicable();
    AddOutput(kOutputs,
              "The output sequence of RNN. The sequence length must be same")
        .AsDuplicable();
    AddOutput(kStepScopes,
              "StepScopes contains all local variables in each time step.");
    AddAttr<std::vector<std::string>>(kExStates,
                                      string::Sprintf(
                                          R"DOC(The ex-state variable names.
The ex-state means the state value in the ex-timestep or the previous time step
[%s, %s, %s] must be the same order)DOC",
                                          kExStates, kStates, kInitStateGrads));
    AddAttr<std::vector<std::string>>(
        kStates,
        string::Sprintf(
            "The state variable names. [%s, %s, %s] must be the same order",
            kExStates, kStates, kInitStateGrads));
    AddAttr<framework::BlockDescBind *>(kStepBlock,
                                        "The step block inside RNN");
    AddAttr<bool>(kReverse, R"DOC(Calculate RNN reversely or not.
By default reverse=False

Assume the input data is [A, B, C, D]

if reverse is False:
  the computation of RNN is like
      A          B          C         D
      |          |          |         |
      v          v          v         v
     rnn -----> rnn -----> rnn ----> rnn
      |          |          |         |
      v          v          v         v
      o          o          o         o

if reverse is True
  the computation of RNN is like
      A          B          C         D
      |          |          |         |
      v          v          v         v
     rnn <----- rnn <----- rnn <---- rnn
      |          |          |         |
      v          v          v         v
      o          o          o         o
)DOC").SetDefault(false);
    AddAttr<bool>(kIsTrain, "").SetDefault(true);
    AddComment(R"DOC(Static Length Recurrent Operator

The static length recurrent operator can only operate on fix sized sequence
data, i.e. in each mini-batch, the sequence length of all inputs are same.
)DOC");
  }
};

class RecurrentGradOpDescMaker : public framework::SingleGradOpDescMaker {
 public:
  using framework::SingleGradOpDescMaker::SingleGradOpDescMaker;

 protected:
  virtual std::unique_ptr<framework::OpDescBind> Apply() const {
    auto *grad = new framework::OpDescBind();
    grad->SetType("recurrent_grad");
    for (auto &input_param : this->InputNames()) {
      grad->SetInput(input_param, this->Input(input_param));
      grad->SetOutput(framework::GradVarName(input_param),
                      this->InputGrad(input_param));
    }

    for (auto &output_param : this->OutputNames()) {
      if (output_param == kStepScopes) {
        grad->SetInput(output_param, this->Output(output_param));
        grad->SetInput(framework::GradVarName(output_param),
                       this->Output(output_param));
      } else {
        grad->SetInput(output_param, this->Output(output_param));
        grad->SetInput(framework::GradVarName(output_param),
                       this->OutputGrad(output_param));
      }
    }

    grad->SetAttrMap(this->Attrs());

    return std::unique_ptr<framework::OpDescBind>(grad);
  }
};

class RecurrentGradOpShapeInference : public framework::InferShapeBase {
 public:
  void operator()(framework::InferShapeContext *ctx) const override {
    std::vector<std::string> input{kInputs, kInitialStates};
    std::vector<std::string> output{kOutputs};
    for (auto &s : input) {
      PADDLE_ENFORCE(ctx->HasInputs(s));
      PADDLE_ENFORCE(ctx->HasOutputs(framework::GradVarName(s)));
    }
    for (auto &s : output) {
      PADDLE_ENFORCE(ctx->HasInputs(s));
    }

    for (auto &s : input) {
      ctx->SetOutputsDim(framework::GradVarName(s), ctx->GetInputsDim(s));
    }
    if (ctx->HasInputs(kParameters)) {
      PADDLE_ENFORCE(ctx->HasOutputs(framework::GradVarName(kParameters)));
      ctx->SetOutputsDim(framework::GradVarName(kParameters),
                         ctx->GetInputsDim(kParameters));
    }
  }
};

}  // namespace operators
}  // namespace paddle

REGISTER_OPERATOR(recurrent, paddle::operators::RecurrentOp,
                  paddle::operators::RecurrentOpProtoMaker,
                  paddle::operators::RecurrentGradOpDescMaker);
REGISTER_OPERATOR(recurrent_grad, paddle::operators::RecurrentGradOp,
                  paddle::operators::RecurrentGradOpShapeInference);
