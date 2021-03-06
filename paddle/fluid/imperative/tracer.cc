// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/fluid/imperative/tracer.h"

namespace paddle {
namespace imperative {

void CreateGradOp(const framework::OpDesc& op_desc,
                  const std::unordered_set<std::string>& no_grad_set,
                  const std::vector<framework::BlockDesc*>& grad_sub_block,
                  framework::OpDesc** grad_op_desc,
                  std::unordered_map<std::string, std::string>* grad_to_var) {
  std::vector<std::unique_ptr<framework::OpDesc>> grad_op_descs =
      framework::OpInfoMap::Instance()
          .Get(op_desc.Type())
          .GradOpMaker()(op_desc, no_grad_set, grad_to_var, grad_sub_block);
  PADDLE_ENFORCE(grad_op_descs.size() == 1, "Only support 1 grad op now.");
  // TODO(panyx0718): Leak?
  *grad_op_desc = grad_op_descs[0].release();
}

void InitVar(framework::Variable* var, framework::Variable* grad_var) {
  auto& var_t = var->Get<framework::LoDTensor>();
  float* data =
      grad_var->GetMutable<framework::LoDTensor>()->mutable_data<float>(
          var_t.dims(), platform::CPUPlace());
  std::fill(data, data + var_t.numel(), 0.0);
}

void Tracer::Trace(OpBase* op, const VarBasePtrMap& inputs,
                   const VarBasePtrMap& outputs, framework::BlockDesc* block,
                   const bool stop_gradient) {
  std::map<std::string, VarBase*> vars;

  framework::OpDesc* op_desc = op->op_desc_;
  VLOG(3) << "tracer tracing " << op_desc->Type();
  op_desc->InferShape(*block);
  op_desc->InferVarType(block);
  std::unique_ptr<framework::OperatorBase> op_base =
      framework::OpRegistry::CreateOp(*op_desc);

  framework::VariableValueMap invars_map;
  framework::VariableValueMap outvars_map;

  op->input_vars_ = inputs;
  for (auto it : op->input_vars_) {
    auto& invars = invars_map[it.first];
    for (VarBase* inp : it.second) {
      PADDLE_ENFORCE_NOT_NULL(inp->var_, "op %s input %s nullptr",
                              op->op_desc_->Type(), inp->var_desc_->Name());

      invars.push_back(inp->var_);
      vars[inp->var_desc_->Name()] = inp;
      if (inp->pre_op_) {
        op->pre_ops_[it.first].push_back(inp->pre_op_);
        op->pre_ops_out_idx_[it.first].push_back(inp->pre_op_out_idx_);
      } else {
        op->pre_ops_[it.first].push_back(nullptr);
      }
      VLOG(3) << "input vname " << inp->var_desc_->Name() << " "
              << inp->var_->IsInitialized();
    }
  }

  op->output_vars_ = outputs;
  for (auto it : op->output_vars_) {
    auto& outvars = outvars_map[it.first];
    const std::vector<VarBase*>& outputs = it.second;
    for (size_t i = 0; i < outputs.size(); ++i) {
      VarBase* out = outputs[i];
      outvars.push_back(out->var_);
      vars[out->var_desc_->Name()] = out;

      framework::VarDesc* var_desc = block->FindVar(out->var_desc_->Name());
      if (var_desc->GetType() == framework::proto::VarType::LOD_TENSOR) {
        out->var_->GetMutable<framework::LoDTensor>();
      } else {
        LOG(ERROR) << "tracer doesn't support yet";
      }
      out->stop_gradient_ = stop_gradient;
      out->pre_op_ = op;
      out->pre_op_out_name_ = it.first;
      out->pre_op_out_idx_ = i;

      VLOG(3) << "output vname " << out->var_desc_->Name() << " "
              << out->var_->IsInitialized();
    }
  }

  VLOG(3) << "tracer running " << op_desc->Type();
  framework::RuntimeContext ctx(invars_map, outvars_map);

  // TODO(panyx0718): Cache p.
  framework::OperatorWithKernel* op_kernel =
      dynamic_cast<framework::OperatorWithKernel*>(op_base.get());
  PADDLE_ENFORCE_NOT_NULL(op_kernel, "only support op with kernel");

  framework::Scope scope;
  platform::CPUPlace place;
  PreparedOp p = PreparedOp::Prepare(ctx, *op_kernel, place);
  p.op.RuntimeInferShape(scope, place, ctx);
  p.func(framework::ExecutionContext(p.op, scope, *p.dev_ctx, p.ctx));

  if (!stop_gradient) {
    framework::OpDesc* grad_op_desc;
    // TODO(panyx): Is this leaked?
    std::unique_ptr<std::unordered_map<std::string, std::string>> grad_to_var(
        new std::unordered_map<std::string, std::string>());
    CreateGradOp(*op_desc, {}, {block}, &grad_op_desc, grad_to_var.get());
    op->grad_op_desc_ = grad_op_desc;

    for (auto it : grad_op_desc->Inputs()) {
      auto& grad_in_vars = op->grad_input_vars_[it.first];
      for (const std::string& grad_invar : it.second) {
        block->FindRecursiveOrCreateVar(grad_invar);
        auto var_it = grad_to_var->find(grad_invar);
        if (var_it == grad_to_var->end()) {
          auto fwd_var_it = vars.find(grad_invar);
          PADDLE_ENFORCE(fwd_var_it != vars.end());
          // Forward inputs or outputs.
          grad_in_vars.push_back(fwd_var_it->second->var_);
        } else {
          VarBase* var = vars[var_it->second];
          if (!var->grads_->var_->IsInitialized()) {
            InitVar(var->var_, var->grads_->var_);
          }
          // Douts.
          grad_in_vars.push_back(var->grads_->var_);
        }
      }
    }

    for (auto it : grad_op_desc->Outputs()) {
      auto& grad_out_vars = op->grad_output_vars_[it.first];
      for (const std::string& grad_outvar : it.second) {
        block->FindRecursiveOrCreateVar(grad_outvar);
        auto var_it = grad_to_var->find(grad_outvar);
        PADDLE_ENFORCE(var_it != grad_to_var->end());
        VarBase* var = vars[var_it->second];
        if (!var->grads_->var_->IsInitialized()) {
          InitVar(var->var_, var->grads_->var_);
        }
        grad_out_vars.push_back(var->grads_->var_);
      }
    }
  }

  op->block_ = block;
}

std::vector<VarBase*> Tracer::PyTrace(OpBase* op,
                                      const std::vector<VarBase*>& inputs,
                                      bool stop_gradient) {
  VLOG(3) << "py_trace";
  op->input_vars_["X"] = inputs;
  op->output_vars_["Out"] = PyLayer::Apply(op->forward_id_, inputs);
  for (VarBase* inp : inputs) {
    if (inp->pre_op_) {
      op->pre_ops_["X"].push_back(inp->pre_op_);
      op->pre_ops_out_idx_["X"].push_back(inp->pre_op_out_idx_);
    } else {
      op->pre_ops_["X"].push_back(nullptr);
    }
  }

  auto& outputs = op->output_vars_["Out"];
  for (size_t i = 0; i < outputs.size(); ++i) {
    VarBase* out = outputs[i];
    out->stop_gradient_ = stop_gradient;
    out->pre_op_ = op;
    out->pre_op_out_name_ = "Out";
    out->pre_op_out_idx_ = i;
  }
  if (!stop_gradient) {
    auto& grad_input_vars = op->grad_input_vars_["X@GRAD"];
    auto& grad_output_vars = op->grad_output_vars_["Out@GRAD"];

    for (const VarBase* inp : inputs) {
      grad_input_vars.push_back(inp->var_);
    }
    for (VarBase* out : outputs) {
      grad_input_vars.push_back(out->var_);
    }
    for (VarBase* out : outputs) {
      grad_input_vars.push_back(out->grads_->var_);
      if (!grad_input_vars.back()->IsInitialized()) {
        InitVar(out->var_, grad_input_vars.back());
      }
    }
    for (const VarBase* inp : inputs) {
      grad_output_vars.push_back(inp->grads_->var_);
      if (!grad_output_vars.back()->IsInitialized()) {
        InitVar(inp->var_, grad_output_vars.back());
      }
    }
  }
  return outputs;
}

}  // namespace imperative
}  // namespace paddle
