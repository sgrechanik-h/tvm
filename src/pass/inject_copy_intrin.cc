/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \brief Replace certain copy with copy intrinsics.
 * \file copy_intrin_rewrite.cc
 */
#include <tvm/ir.h>
#include <tvm/packed_func_ext.h>
#include <tvm/ir_mutator.h>
#include <tvm/ir_pass.h>
#include "../arithmetic/pattern_match.h"

namespace tvm {
namespace ir {

using runtime::PackedFunc;

class CopyIntrinInjector : public IRMutator {
 public:
  CopyIntrinInjector(const std::string& pragma_key,
                     const PackedFunc& flower_copy_fromto)
      : pragma_key_(attr::pragma_scope_prefix+  pragma_key),
        flower_copy_fromto_(flower_copy_fromto) {
  }

  Stmt Mutate_(const AttrStmt* op, const Stmt& s) final {
    if (op->attr_key == attr::storage_scope) {
      const Variable* buf = op->node.as<Variable>();
      storage_scope_[buf] = op->value.as<StringImm>()->value;
    } else if (op->attr_key == pragma_key_) {
      Stmt ret;
      CHECK(MatchCopyPattern(op->body, &ret))
          << "Cannot match copy pattern of " << op->body;
      return ret;
    }
    return IRMutator::Mutate_(op, s);
  }

 private:
  bool MatchCopyPattern(Stmt stmt, Stmt *out) {
    using namespace arith;
    Stmt body = stmt;

    // strip the loops
    std::vector<const For*> loops;
    while (const For* op = body.as<For>()) {
      if (!is_zero(op->min)) return false;
      loops.push_back(op);
      body = op->body;
    }
    const Store* store = body.as<Store>();
    if (store == nullptr) return false;
    // Expr sel_cond, sel_true_value, sel_false_value;
    // match select or if
    PVar<Expr> sel_cond, sel_true_value, sel_false_value;
    bool has_cond =
        if_then_else(sel_cond, sel_true_value, sel_false_value).Match(store->value) ||
        select(sel_cond, sel_true_value, sel_false_value).Match(store->value);

    const Cast* cast = store->value.as<Cast>();
    const Load* load = store->value.as<Load>();
    if (0 == loops.size()) {
      CHECK(!has_cond);
    }
    // for now only support true condition matching
    if (has_cond) {
      load = sel_true_value.Eval().as<Load>();
    }
    // cast can be part of the pattern
    if (cast != nullptr) {
      load = cast->value.as<Load>();
    }
    if (load == nullptr) return false;
    if (load->type.lanes() != 1) return false;
    Array<Var> loop_vars;
    for (const For* op : loops) {
      loop_vars.push_back(op->loop_var);
    }
    Array<Expr> store_strides =
        arith::DetectLinearEquation(store->index, loop_vars);
    Array<Expr> load_strides =
        arith::DetectLinearEquation(load->index, loop_vars);
    if (load_strides.size()  == 0 || store_strides.size() == 0) return false;
    Array<Expr> dst_shape;
    const size_t loop_var_size = loop_vars.size();
    if (loop_var_size == 0) {
      dst_shape.push_back(make_const(Int(32), 1));
    } else {
      for (const For* op : loops) {
        dst_shape.push_back(op->extent);
      }
    }
    Array<Expr> src_shape = dst_shape;
    Array<Expr> pad_before, pad_after;
    Expr pad_value;
    Expr src_elem_offset = load_strides[loop_var_size];
    if (has_cond) {
      Array<Expr> clip_bound =
          arith::DetectClipBound(sel_cond.Eval(), loop_vars);
      pad_value = sel_false_value.Eval();
      if (clip_bound.size() == 0) return false;
      CHECK_EQ(src_shape.size(), loop_vars.size());
      CHECK_EQ(clip_bound.size(), loop_vars.size() * 2);
      for (size_t i = 0; i < src_shape.size(); ++i) {
        Expr min_value = clip_bound[2 * i];
        Expr max_value = clip_bound[2 * i + 1];
        Type t = loop_vars[i].type();
        Expr svalue = src_shape[i];
        if (min_value.defined()) {
          Expr pbefore = Simplify(Max::make(min_value, make_zero(t)));
          src_elem_offset = src_elem_offset + pbefore * load_strides[i];
          svalue = svalue - pbefore;
          pad_before.push_back(pbefore);
        } else {
          pad_before.push_back(make_zero(t));
        }
        if (max_value.defined()) {
          Expr pafter = Simplify(Max::make(loops[i]->extent - max_value - make_const(t, 1),
                                           make_zero(t)));
          svalue = svalue - pafter;
          pad_after.push_back(pafter);
        } else {
          pad_after.push_back(make_zero(t));
        }
        src_shape.Set(i, Simplify(svalue));
      }
      src_elem_offset = Simplify(src_elem_offset);
    }
    CHECK_EQ(load_strides.size(), store_strides.size());
    CHECK_EQ(load_strides.size(), loop_var_size + 1);
    Array<Expr> src_strides(load_strides.begin(), load_strides.begin() + loop_var_size);
    Array<Expr> dst_strides(store_strides.begin(), store_strides.begin() + loop_var_size);
    if (loop_var_size == 0) {
        src_strides.push_back(make_const(Int(32), 1));
        dst_strides.push_back(make_const(Int(32), 1));
    }
    Buffer dst = BufferNode::make(
        store->buffer_var,
        store->value.type(),
        dst_shape,
        dst_strides,
        store_strides[loop_var_size],
        store->buffer_var->name_hint,
        GetStorageScope(store->buffer_var.get()),
        0, 0, kDefault);
    Buffer src = BufferNode::make(
        load->buffer_var,
        load->type,
        src_shape,
        src_strides,
        src_elem_offset,
        load->buffer_var->name_hint,
        GetStorageScope(load->buffer_var.get()),
        0, 0, kDefault);
    *out = flower_copy_fromto_(src, dst, pad_before, pad_after, pad_value);
    CHECK(out->defined()) << "flower function did not return correct stmt";
    return true;
  }
  // Get storage scope
  std::string GetStorageScope(const Variable* var) const {
    auto it = storage_scope_.find(var);
    if (it != storage_scope_.end()) {
      return it->second;
    } else {
      return "";
    }
  }
  // pragma key
  std::string pragma_key_;
  // function to lower copy intrinsics.
  const PackedFunc& flower_copy_fromto_;
  // Storage scope
  std::unordered_map<const Variable*, std::string> storage_scope_;
};

Stmt InjectCopyIntrin(Stmt stmt,
                      const std::string& pragma_key,
                      const PackedFunc& flower_copy_fromto) {
  return CopyIntrinInjector(pragma_key, flower_copy_fromto)
      .Mutate(stmt);
}

}  // namespace ir
}  // namespace tvm
