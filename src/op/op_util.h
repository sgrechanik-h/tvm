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
 * \file op_util.h
 * \brief Common utility used in operator construction.
 */
#ifndef TVM_OP_OP_UTIL_H_
#define TVM_OP_OP_UTIL_H_

#include <tvm/expr.h>
#include <tvm/schedule.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include "../pass/ir_util.h"
#include "../pass/arg_binder.h"
#include "../schedule/message_passing.h"

namespace tvm {
namespace op {

using ir::MergeNest;

/*!
 * \brief Build loop nest for stage.
 *
 * \param stage The stage to create a loop nest.
 * \param dom_map The range of each iter var.
 * \param begin_iter_pos The beginning position of leaf_iter_vars to generate loop.
 * \param new_loop_var Whether create new loop variable.
 * \param skip_iter Whether skip certain iteration.
 * \param p_value_map The result value of each IterVar.
 * \param debug_keep_trivial_loop Whether keep trivial loops with extent of 1
 */
std::vector<std::vector<Stmt> >
MakeLoopNest(const Stage& stage,
             const std::unordered_map<IterVar, Range>& dom_map,
             size_t begin_iter_pos,
             bool new_loop_var,
             const std::unordered_set<IterVar>& skip_iter,
             std::unordered_map<IterVar, Expr>* p_value_map,
             bool debug_keep_trivial_loop);

/*!
 * \brief Create a nest of if checking the predicates.
 *
 * \param predicates The predicates to be checked.
 * \return List of If nest that checks the predicates.
 */
std::vector<Stmt> MakeIfNest(const std::vector<Expr>& predicates);

/*!
 * \brief Replace the tensor reference (especially in Call's) in stmt by the replace map.
 * \param stmt The statement to be processed.
 * \param replace The replacement rule.
 */
Stmt ReplaceTensor(Stmt stmt,
                   const std::unordered_map<Tensor, Tensor>& replace);
/*!
 * \brief Replace the tensor reference (especially in Call's) in stmt by the replace map.
 * \param expr The expression to be processed.
 * \param replace The replacement rule.
 */
Expr ReplaceTensor(Expr expr,
                   const std::unordered_map<Tensor, Tensor>& replace);

/*!
 * \brief Replace tensor references in the given tensors recursively (not only in their bodies
 *  but also in the bodies of its dependencies).
 * \param tensors The tensors to be processed.
 * \param replace The replacement rule.
 */
Array<Tensor> ReplaceTensorRecursively(Array<Tensor> tensors,
                                       const std::unordered_map<Tensor, Tensor>& replace);

/*!
 * \brief Substitute the variables of stmt by value map.
 * \param stmt the statment
 * \param value_map The value map.
 * \return Substituted result.
 */
Stmt Substitute(Stmt stmt,
                const std::unordered_map<IterVar, Expr>& value_map);

/*!
 * \brief Converts Halide ForType to its corresponding IterVarType
 * \param for_type The ForType to be converted
 */
IterVarType ForTypeToIterVarType(ir::ForType for_type);

/*!
 * \brief Converts IterVarType to its corresponding Halide ForType
 * \param iter_type The IterVarType to be converted
 */
ir::ForType IterVarTypeToForType(IterVarType iter_type);

// TODO: Move these functions to some tensor_util or ir_util
/*!
 * \brief Clone reduction by cloning the axis variables.
 * \param expr A reduction expr to clone. Non-reduction expressions are left intact.
 */
Expr CloneReduction(const Expr& expr);

/*!
 * \brief Create a compute op from expressions. If the expressions contain a single reduction,
 *  its body will be correctly duplicated if it is a multi-valued reduction.
 *
 * \param exprs The expr which will be the tensor's body.
 * \param axis The input variables with ranges.
 * \param name The operation's name.
 * \param tag The operation's tag.
 * \param attrs The operation's attrs.
 * \param clone_axis Whether to clone the given axis and perform substitution.
 * \return A tensor.
 */
Operation ComputeOpFromExprs(const Array<Expr>& exprs, const Array<IterVar>& axis,
                             const std::string& name, const std::string& tag,
                             const Map<std::string, NodeRef>& attrs,
                             bool clone_axis = true);

/*!
 * \brief Create a tensor from an expression. The expression may be a reduction, in which
 *  case its body will be correctly duplicated if it is a multi-valued reduction.
 *
 * \param expr The expr which will be the tensor's body.
 * \param axis The input variables with ranges.
 * \param name The tensor's name.
 * \param tag The tensor's tag.
 * \param attrs The tensor's attrs.
 * \param clone_axis Whether to clone the given axis and perform substitution.
 * \return A tensor.
 */
Tensor TensorFromExpr(const Expr& expr, const Array<IterVar>& axis,
                      const std::string& name = "tensor", const std::string& tag = "",
                      const Map<std::string, NodeRef>& attrs = {},
                      bool clone_axis = true);

/*!
 * \brief Transform the body of a tensor if it is a compute tensor, otherwise return it
 *  unchanged. Note that if the compute returns a tuple, it transforms only one element,
 *  other elements are discarded.
 *
 * \param tensor The tensor to transform.
 * \param func The transformation function working on expressions and additionally taking
 *  the array of the tensor's itervars.
 * \return The transformed tensor.
 */
Tensor TransformBody(const Tensor& tensor,
                     std::function<Expr(const Expr&, const Array<IterVar>&)> func);

/*!
 * \brief Transform the body of a tensor if it is a compute tensor, otherwise return it
 *  unchanged. Note that if the compute returns a tuple, it transforms only one element,
 *  other elements are discarded.
 *
 * \param tensor The tensor to transform.
 * \param func The transformation function (working on expressions).
 * \return The transformed tensor.
 */
Tensor TransformBody(const Tensor& tensor, std::function<Expr(const Expr&)> func);

}  // namespace op
}  // namespace tvm
#endif  // TVM_OP_OP_UTIL_H_
