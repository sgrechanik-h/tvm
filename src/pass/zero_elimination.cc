/*!
 *  Copyright (c) 2018 by Contributors
 * \file zero_elimination.cc
 * \brief Transform tensors in such a way as to eliminate summation over zeros.
 */
#include "zero_elimination.h"

#include <tvm/api_registry.h>
#include <tvm/ir_functor_ext.h>
#include <tvm/ir.h>
#include <tvm/ir_mutator.h>
#include <tvm/ir_pass.h>
#include <tvm/ir_visitor.h>
#include <tvm/operation.h>
#include <dmlc/optional.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <tuple>

#include "../op/op_util.h"

// Uncomment this line to make zero elimination very verbose
//#define ZERO_ELIMINATION_VERBOSE true
#ifdef ZERO_ELIMINATION_VERBOSE
  #define ZE_LOG_NL() (std::cout << std::endl)
  #define ZE_LOG(text, value) \
    ZELog(std::string(__FUNCTION__) + ": " + text, (value))
  #define ZE_LOG_VAR(var) ZE_LOG(#var, var)
  #define ZE_LOG_RES(value) \
    ZELog(std::string(__FUNCTION__) + " returned", (value))
  #define ZE_LOG_ENTER() auto _ze_log_scope = ZELogScope(__FUNCTION__)
#else
  #define ZE_LOG_NL()
  #define ZE_LOG(text, value)
  #define ZE_LOG_VAR(var)
  #define ZE_LOG_RES(value) (value)
  #define ZE_LOG_ENTER()
#endif

namespace tvm {
namespace ir {

// Convert a variable map into a sorted vector of pairs. Sorting is done with deep expr comparison.
template <typename T>
std::vector<std::pair<Var, T>> VarMapToVectorOfPairs(const Map<Var, T>& varmap) {
  using tpair = std::pair<Var, T>;
  std::vector<tpair> res;
  for (const tpair& pair : varmap) {
    res.push_back(pair);
  }
  std::sort(res.begin(), res.end(),
            [](const tpair& l, const tpair& r) { return Compare(l.first, r.first) < 0; });
  return res;
}

template <typename T>
struct PrintSortedVarMapImpl {
  const Map<Var, T>& varmap_;
  PrintSortedVarMapImpl(const Map<Var, T>& varmap) : varmap_(varmap) {}
};

template <typename T>
std::ostream& operator<<(std::ostream& stream, const PrintSortedVarMapImpl<T>& varmap) {
  stream << "{";
  bool first_iteration = true;
  for (const auto& pair : VarMapToVectorOfPairs(varmap.varmap_)) {
    if (!first_iteration) {
      stream << ", ";
    }
    stream << pair.first << ": " << pair.second;
    first_iteration = false;
  }
  stream << "}";
  return stream;
}

// Print a variable map as a map sorted by variables
// Usage: std::cout << PrintSortedVarMap(varmap);
template <typename T>
PrintSortedVarMapImpl<T> PrintSortedVarMap(const Map<Var, T>& varmap) {
  return PrintSortedVarMapImpl<T>(varmap);
}

#ifdef ZERO_ELIMINATION_VERBOSE
  thread_local size_t ze_log_shift = 0;

  class ZELogScope {
    public:
      std::string function_name;
      explicit ZELogScope(const std::string& fname) : function_name(fname) {
        std::cout << std::endl << std::string(ze_log_shift*2, ' ') << fname << " {" << std::endl;
        ++ze_log_shift;
      }
      ~ZELogScope() {
        --ze_log_shift;
        std::cout << std::string(ze_log_shift*2, ' ')
          << "} end " << function_name << std::endl << std::endl;
      }
  };

  template <typename tvalue>
  tvalue ZELog(const std::string& message, tvalue&& val) {
    std::cout << std::string(ze_log_shift*2, ' ')
      << message << " " << val << std::endl;
    return val;
  }

  template <class tvalue>
  Map<Var, tvalue> ZELog(const std::string& message, const Map<Var, tvalue>& val) {
    std::cout << std::string(ze_log_shift*2, ' ')
      << message << " " << PrintSortedVarMap(val) << std::endl;
    return val;
  }
#endif

bool DoBadThings() {
  static int step = 0;
  static int bad_start = 0, bad_end = 0;
  if (step == 0) {
    if (getenv("TVM_ZE_BAD_START")) {
      bad_start = std::atoi(getenv("TVM_ZE_BAD_START"));
    }
    if (getenv("TVM_ZE_BAD_END")) {
      bad_end = std::atoi(getenv("TVM_ZE_BAD_END"));
    }
  }
  ++step;
  ZE_LOG("step", step);
  if (step >= bad_start && step < bad_end) {
    ZE_LOG("Doing bad things!", "");
    return true;
  } else {
    return false;
  }
}

int gcd(int a, int b) {
    if (a < b) std::swap(a, b);
    while (b != 0) {
        int64_t tmp = b;
        b = a % b;
        a = tmp;
    }
    return a;
}

int lcm(int a, int b) {
    return (a*b)/gcd(a, b);
}

struct ExprLess {
    bool operator()(const Expr& l, const Expr& r) const {
      return Compare(l, r) < 0;
    }
};

struct ExprEq {
    bool operator()(const Expr& l, const Expr& r) const {
      return Compare(l, r) == 0;
    }
};

// Merge two maps, prefer the right one on conflict
template <class K, class V>
Map<K, V> Merge(Map<K, V> original, const Map<K, V>& update) {
  for (const auto& p : update) {
    original.Set(p.first, p.second);
  }
  return std::move(original);
}

// Concatenate two arrays
template <class T>
Array<T> Concat(Array<T> a, const Array<T>& b) {
  for (const auto& x : b) {
    a.push_back(x);
  }
  return std::move(a);
}

// Combine all expressions from the container using &&.
template <class container>
Expr All(const container& c) {
  Expr res;
  for (const auto& e : c) {
    if (res.get()) {
      res = res && e;
    } else {
      res = e;
    }
  }
  if (res.get()) {
    return res;
  } else {
    return const_true();
  }
}

// Create a select statement of the form cond ? on_true : 0
Expr SelectElseZero(const Expr& cond, const Expr& on_true) {
  return Select::make(cond, on_true, make_zero(on_true.type()));
}

// Simplify the expression as thoroughly as possible by using all available simplifiers.
Expr SuperSimplify(Expr e, const Map<Var, Range>& vranges = Map<Var, Range>()) {
  // For some reason no simplifier can detect that there is only one value of the variable
  std::unordered_map<const Variable*, Expr> vmap;
  for (const auto& var_range : vranges) {
    if (is_const_int(var_range.second->extent, 1)) {
      vmap[var_range.first.get()] = var_range.second->min;
    }
  }
  if (!vmap.empty()) {
    e = Substitute(e, vmap);
  }

  arith::Analyzer an;
  for (const auto& var_range : vranges) {
    an.Bind(var_range.first, var_range.second);
  }

  // According to my experiments two best simplifications orders were can->rw and rw->can->rw,
  // but rw->can->rw is better for a couple of cases.
  // Note that we should end with rw because it factors multipliers out.
  Expr res = e;
  res = an.rewrite_simplify(res);
  res = an.canonical_simplify(res);
  res = an.rewrite_simplify(res);

  return res;
}

// Provability check that uses SuperSimplify
bool CanProve(Expr e, const Map<Var, Range>& vranges = Map<Var, Range>()) {
  return is_one(SuperSimplify(e, vranges));
}

class ExprFreeVarsVisitor : public IRVisitor {
 public:
  std::vector<Var> free_array;
  std::unordered_set<const Variable*> bound;
  std::unordered_set<const Variable*> free;

  virtual void Visit(const NodeRef& node) {
    if (const Variable* v = node.as<Variable>()) {
      if (!bound.count(v) && !free.count(v)) {
        free.insert(v);
        free_array.push_back(Downcast<Var>(node));
      }
    } else {
      IRVisitor::Visit(node);
    }
  }

  void Visit_(const Variable* op) {
    CHECK(false) << "This case shouldn't happen";
  }

  void Visit_(const LetStmt* op) {
    bound.insert(op->var.get());
    IRVisitor::Visit_(op);
  }

  void Visit_(const For* op) {
    bound.insert(op->loop_var.get());
    IRVisitor::Visit_(op);
  }

  void Visit_(const Let* op) {
    bound.insert(op->var.get());
    IRVisitor::Visit_(op);
  }

  void Visit_(const Reduce* op) {
    for (const auto& iv : op->axis) {
      bound.insert(iv->var.get());
    }
    IRVisitor::Visit_(op);
  }

  void Visit_(const Store* op) {
    Visit(op->buffer_var);
    IRVisitor::Visit_(op);
  }

  void Visit_(const Allocate* op) {
    Visit(op->buffer_var);
    IRVisitor::Visit_(op);
  }

  void Visit_(const Free* op) {
    Visit(op->buffer_var);
    IRVisitor::Visit_(op);
  }

  void Visit_(const Load* op) {
    Visit(op->buffer_var);
    IRVisitor::Visit_(op);
  }
};

// Get free variables of an expression
Array<Var> ExprFreeVars(const Expr& expr) {
  ExprFreeVarsVisitor visitor;
  visitor.Visit(expr);
  return visitor.free_array;
}

DomainTransformation ComposeDomainTransformations(const DomainTransformation& first,
                                                  const DomainTransformation& second) {
  CHECK(second->old_domain.same_as(first->new_domain));
  Map<Var, Expr> new_to_old;
  Map<Var, Expr> old_to_new;
  for (auto p : second->new_to_old) {
    new_to_old.Set(p.first, SuperSimplify(Substitute(p.second, first->new_to_old),
                                          first->old_domain->ranges));
  }
  for (auto p : first->old_to_new) {
    old_to_new.Set(p.first, SuperSimplify(Substitute(p.second, second->old_to_new),
                                          second->new_domain->ranges));
  }
  return DomainTransformationNode::make(second->new_domain, first->old_domain,
                                        new_to_old, old_to_new);
}

DomainTransformation DomainTransformation::operator+=(const DomainTransformation& other) {
  *this = ComposeDomainTransformations(*this, other);
  return *this;
}

DomainTransformation EmptyDomainTransformation(const Domain& domain) {
  Map<Var, Expr> new_to_old;
  Map<Var, Expr> old_to_new;
  for (const Var& v : domain->variables) {
    old_to_new.Set(v, make_zero(v.type()));
  }
  Domain new_domain = DomainNode::make({}, {make_zero(Bool())}, {});
  return DomainTransformationNode::make(new_domain, domain, new_to_old, old_to_new);
}

DomainTransformation IdDomainTransformation(const Domain& domain) {
  Map<Var, Expr> new_to_old;
  for (const Var& v : domain->variables) {
    new_to_old.Set(v, v);
  }
  return DomainTransformationNode::make(domain, domain, new_to_old, new_to_old);
}

// Convert an array of itervars to an array of inequalities
Array<Expr> IterVarsToInequalities(const Array<IterVar>& itervars) {
  Array<Expr> res;
  for (const IterVar& v : itervars) {
    res.push_back(GE::make(v->var, v->dom->min));
    res.push_back(LT::make(v->var, v->dom->min + v->dom->extent));
  }
  return res;
}

// Convert an array of itervars to a map from vars to ranges
Map<Var, Range> IterVarsToMap(const Array<IterVar>& itervars) {
  Map<Var, Range> res;
  for (const IterVar& v : itervars) {
    res.Set(v->var, v->dom);
  }
  return res;
}

// Convert an array of itervars to an array of vars
Array<Var> IterVarsToVars(const Array<IterVar>& itervars) {
  Array<Var> res;
  for (const IterVar& v : itervars) {
    res.push_back(v->var);
  }
  return res;
}

// Given a map from vars to ranges create an array of itervars
Array<IterVar> IterVarsFromMap(const Array<Var>& vars, const Map<Var, Range>& vranges,
                               IterVarType iter_type = kDataPar, std::string thread_tag = "") {
  Array<IterVar> res;
  for (const Var& v : vars) {
    CHECK(vranges.count(v)) << "A range for the variable " << v
      << " was not provided in map " << vranges;
    res.push_back(IterVarNode::make(vranges[v], v, iter_type, thread_tag));
  }
  return res;
}

// Return true if this combiner is just a sum.
bool IsSumCombiner(const CommReducer& combiner, const Map<Var, Range>& vranges) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(combiner);
  ZE_LOG_VAR(vranges);

  if (combiner->result.size() != 1) {
    return ZE_LOG_RES(false);
  }

  if (!is_const_value(SuperSimplify(combiner->identity_element[0], vranges), 0)) {
    return ZE_LOG_RES(false);
  }

  Expr combiner_result = SuperSimplify(combiner->result[0], vranges);

  return ZE_LOG_RES(Equal(combiner_result, combiner->lhs[0] + combiner->rhs[0]) ||
                    Equal(combiner_result, combiner->rhs[0] + combiner->lhs[0]));
}

// Return true if zero may be factored out of a reduction with this combiner.
bool CanFactorZeroFromCombiner(const CommReducer& combiner, int value_index,
                               const Map<Var, Range>& vranges) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(combiner);
  ZE_LOG_VAR(value_index);
  ZE_LOG_VAR(vranges);

  if (!is_const_value(SuperSimplify(combiner->identity_element[value_index], vranges), 0)) {
    return ZE_LOG_RES(false);
  }

  Expr zero = make_zero(combiner->result[value_index].type());
  Expr in = Substitute(combiner->result[value_index],
                       {{combiner->lhs[value_index], zero},
                        {combiner->rhs[value_index], zero}});
  in = SuperSimplify(in, vranges);

  return ZE_LOG_RES(is_const_value(in, 0));
}

// If expr is a Call node, perform inlining, otherwise do nothing
Expr InlineThisCall(const Expr& expr) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);

  if (const Call* op = expr.as<Call>()) {
    if (op->call_type == Call::CallType::Halide) {
      if (const ComputeOpNode* op_comp = op->func.as<ComputeOpNode>()) {
        Array<Var> tensor_axes;
        for (const auto& var : op_comp->axis) {
          tensor_axes.push_back(var->var);
        }

        Stmt inlined = Inline(Evaluate::make(expr), op->func, tensor_axes,
                              op_comp->body[op->value_index]);
        if (const ir::Evaluate* ev = inlined.as<ir::Evaluate>()) {
          // If it is a reduction, clone it
          return ZE_LOG_RES(op::CloneReduction(ev->value));
        }
      }
    }
  }

  return ZE_LOG_RES(expr);
}

Tensor InlineTailCall(const Tensor& tensor) {
  return op::TransformBody(tensor, InlineThisCall);
}

// Implements InlineTensors by trying to inline every Call of the given Expr
class InlineTensorsMutator : public IRMutator {
 public:
  explicit InlineTensorsMutator(const Array<Tensor>& inlineable, bool inline_reductions = false)
      : inline_reductions_(inline_reductions) {
    for (const Tensor& tensor : inlineable) {
      inlineable_.emplace(tensor->op.operator->(), tensor->value_index);
    }
  }

  Expr Mutate_(const Call* op, const Expr& e) {
    if (op->call_type == Call::CallType::Halide) {
      if (const ComputeOpNode* op_comp = op->func.as<ComputeOpNode>()) {
        // Inline only if the array of inlineable tensors is empty or contains this tensor
        if (inlineable_.empty() || inlineable_.count({op_comp, op->value_index})) {
          // Inline only compute nodes that are not reductions (unless inline reductions is allowed)
          if (inline_reductions_ || !op_comp->body[0].as<Reduce>()) {
            // Inline this call and then try to perform further inlining
            return Mutate(InlineThisCall(e));
          }
        }
      }
    }

    // If we cannot inline this call, we should try to do inlining in its arguments
    return IRMutator::Mutate_(op, e);
  }

 private:
  // Tensors which are allowed to be inlined, represented as pairs (op_node, value_index)
  std::set<std::pair<const OperationNode*, int>> inlineable_;
  bool inline_reductions_;
};

Expr InlineTensors(const Expr& expr, const Array<Tensor>& inlineable,
                   bool inline_reductions) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);
  ZE_LOG_VAR(inlineable);
  ZE_LOG_VAR(inline_reductions);
  return ZE_LOG_RES(InlineTensorsMutator(inlineable, inline_reductions).Mutate(expr));
}

Tensor InlineTensors(const Tensor& tensor, const Array<Tensor>& inlineable,
                     bool inline_reductions) {
  auto transformation =
    [inlineable, inline_reductions](const Expr& e) {
      return InlineTensorsMutator(inlineable, inline_reductions).Mutate(e); };
  return op::TransformBody(tensor, transformation);
}


struct NonzeronessConditionResult {
  Expr cond;
  Expr value;

  Expr to_expr() const {
    return SelectElseZero(cond, value);
  }

  friend std::ostream& operator<<(std::ostream& os, const NonzeronessConditionResult& r) {
    return os << r.to_expr();
  }
};

// The implementation of NonzeronessCondition
class NonzeronessConditionFunctor
  : public ExprFunctor<NonzeronessConditionResult(const Expr&, const Expr&)> {
 public:
  NonzeronessConditionResult NonzeronessCondition(const Expr& e) {
    if (e.type().is_bool()) {
      // Boolean expressions are non-zero whenever they are true themselves
      return {e, const_true()};
    } else {
      return VisitExpr(e, e);
    }
  }

  // Most of the cases are implemented using helpers below
  result_type VisitExpr_(const Variable*, const Expr& e) final { return Default_(e); }
  result_type VisitExpr_(const IntImm* op, const Expr& e) final { return Const_(op, e); }
  result_type VisitExpr_(const UIntImm* op, const Expr& e) final { return Const_(op, e); }
  result_type VisitExpr_(const FloatImm* op, const Expr& e) final { return Const_(op, e); }
  result_type VisitExpr_(const StringImm*, const Expr& e) final { return Default_(e); }
  result_type VisitExpr_(const Add* op, const Expr& e) final { return BinOpAddLike_(op, e); }
  result_type VisitExpr_(const Sub* op, const Expr& e) final { return BinOpAddLike_(op, e); }
  result_type VisitExpr_(const Mul* op, const Expr& e) final { return BinOpMulLike_(op, e); }
  result_type VisitExpr_(const Div* op, const Expr& e) final { return BinOpDivLike_(op, e); }
  result_type VisitExpr_(const Mod* op, const Expr& e) final { return BinOpDivLike_(op, e); }
  result_type VisitExpr_(const FloorDiv* op, const Expr& e) final { return BinOpDivLike_(op, e); }
  result_type VisitExpr_(const FloorMod* op, const Expr& e) final { return BinOpDivLike_(op, e); }
  result_type VisitExpr_(const Min* op, const Expr& e) final { return BinOpAddLike_(op, e); }
  result_type VisitExpr_(const Max* op, const Expr& e) final { return BinOpAddLike_(op, e); }

  result_type VisitExpr_(const Cast* op, const Expr& e) final {
    auto nz_a = NonzeronessCondition(op->value);

    if (nz_a.value.same_as(op->value)) {
      return {nz_a.cond, e};
    } else {
      return {nz_a.cond, Cast::make(op->type, nz_a.value)};
    }
  }

  result_type VisitExpr_(const Select* op, const Expr& e) final {
    Expr cond = op->condition, true_val = op->true_value, false_val = op->false_value;
    auto nz_a = NonzeronessCondition(true_val);
    auto nz_b = NonzeronessCondition(false_val);

    // If the false part is zero, we can get rid of the select
    if (is_const_value(nz_b.value, 0)) {
      Expr new_cond = SuperSimplify(nz_a.cond && cond);
      return {new_cond, nz_a.value};
    }

    // If the true part is zero, we can also get rid of the select
    if (is_const_value(nz_a.value, 0)) {
      Expr new_cond = SuperSimplify(nz_b.cond && !cond);
      return {new_cond, nz_b.value};
    }

    // Otherwise we retain the select and combine the conditions into this
    Expr new_cond = SuperSimplify((cond && nz_a.cond) || (!cond && nz_b.cond));
    if (nz_a.value.same_as(true_val) && nz_b.value.same_as(false_val)) {
      return {new_cond, e};
    } else {
      return {new_cond, Select::make(cond, nz_a.value, nz_b.value)};
    }
  }

  result_type VisitExpr_(const Call* op, const Expr& e) final {
    if (op->name == intrinsic::tvm_if_then_else) {
      Expr cond = op->args[0], true_val = op->args[1], false_val = op->args[2];
      auto nz_a = NonzeronessCondition(true_val);
      auto nz_b = NonzeronessCondition(false_val);

      // We don't have as much freedom here as in the select case
      // since the `if` must be preserved in any case
      Expr new_cond = SuperSimplify((cond && nz_a.cond) || (!cond && nz_b.cond));
      if (nz_a.value.same_as(true_val) && nz_b.value.same_as(false_val)) {
        return {new_cond, e};
      } else {
        return {new_cond, if_then_else(cond, nz_a.value, nz_b.value)};
      }
    } else {
      return Default_(e);
    }
  }

  NonzeronessConditionResult Default_(const Expr& e) {
    // This is always correct, so it's the default
    return {const_true(), e};
  }

  template <class TNode>
  NonzeronessConditionResult Const_(const TNode* op, const Expr& e) {
    if (op->value == 0) {
      return {const_false(), e};
    } else {
      return {const_true(), e};
    }
  }

  template <class TNode>
  NonzeronessConditionResult BinOpAddLike_(const TNode* op, const Expr& e) {
    auto nz_a = NonzeronessCondition(op->a);
    auto nz_b = NonzeronessCondition(op->b);

    // For addition and similar ops the result may be nonzero if either of the arguments is
    // nonzero, so we combine the conditions with Or.

    if (Equal(nz_a.cond, nz_b.cond)) {
      // If the conditions are the same, we don't need Or
      if (nz_a.value.same_as(op->a) && nz_b.value.same_as(op->b)) {
        return {nz_a.cond, e};
      } else {
        return {nz_a.cond, TNode::make(nz_a.value, nz_b.value)};
      }
    } else {
      // Otherwise use Or
      Expr new_cond = SuperSimplify(nz_a.cond || nz_b.cond);
      // A little optimization: if the combined condition is the same as one of the inner
      // conditions, we don't need to guard the inner value with a select, otherwise
      // we create a select in the `to_expr` call.
      Expr new_a = Equal(nz_a.cond, new_cond) ? nz_a.value : nz_a.to_expr();
      Expr new_b = Equal(nz_b.cond, new_cond) ? nz_b.value : nz_b.to_expr();
      Expr new_expr = TNode::make(new_a, new_b);
      return {new_cond, new_expr};
    }
  }

  template <class TNode>
  NonzeronessConditionResult BinOpMulLike_(const TNode* op, const Expr& e) {
    auto nz_a = NonzeronessCondition(op->a);
    auto nz_b = NonzeronessCondition(op->b);

    // For multiplication and similar ops the result may be nonzero if
    // both the arguments are nonzero, so we combine with And.

    Expr new_cond = SuperSimplify(nz_a.cond && nz_b.cond);

    if (nz_a.value.same_as(op->a) && nz_b.value.same_as(op->b)) {
      return {new_cond, e};
    } else {
      return {new_cond, TNode::make(nz_a.value, nz_b.value)};
    }
  }

  template <class TNode>
  NonzeronessConditionResult BinOpDivLike_(const TNode* op, const Expr& e) {
    auto nz_a = NonzeronessCondition(op->a);

    // For Div we simply use the condition of the numerator.

    if (nz_a.value.same_as(op->a)) {
      return {nz_a.cond, e};
    } else {
      return {nz_a.cond, TNode::make(nz_a.value, op->b)};
    }
  }
};

// Transform expr into a pair (condition, new_expr) such that the old expr is equivalent to
// `select(condition, new_expr, 0)`. The pair is represented as a struct for clarity.
NonzeronessConditionResult NonzeronessCondition(const Expr& expr) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);
  return ZE_LOG_RES(NonzeronessConditionFunctor().NonzeronessCondition(expr));
}

Expr LiftNonzeronessCondition(const Expr& expr) {
  return NonzeronessCondition(expr).to_expr();
}


class NormalizeComparisonsMutator : public IRMutator {
 public:
  virtual Expr Mutate_(const EQ* op, const Expr& e) { return Make<EQ>(op->a, op->b); }
  virtual Expr Mutate_(const NE* op, const Expr& e) { return Make<NE>(op->a, op->b); }
  virtual Expr Mutate_(const LT* op, const Expr& e) { return Make<LT>(op->a, op->b); }
  virtual Expr Mutate_(const LE* op, const Expr& e) { return Make<LE>(op->a, op->b); }
  virtual Expr Mutate_(const GT* op, const Expr& e) { return Make<LT>(op->b, op->a); }
  virtual Expr Mutate_(const GE* op, const Expr& e) { return Make<LE>(op->b, op->a); }

 private:
  template <class TNode>
  Expr Make(const Expr& a, const Expr& b) {
    // rewrite LT to LE for ints
    if (std::is_same<TNode, LT>::value && (a.type().is_int() || a.type().is_uint())) {
      return LE::make(SuperSimplify(a - b + 1), make_zero(a.type()));
    }
    return TNode::make(SuperSimplify(a - b), make_zero(a.type()));
  }
};

// Rewrite every comparison into the form a == 0, a != 0, a <= 0, and sometimes for floats a < 0
Expr NormalizeComparisons(const Expr& expr) {
  return NormalizeComparisonsMutator().Mutate(expr);
}


struct FactorOutAtomicFormulasResult {
  std::vector<Expr> atomic_formulas;
  Expr rest;

  Expr to_expr() const {
    Expr res = rest;
    for (const Expr& e : atomic_formulas) {
      res = And::make(e, res);
    }
    return res;
  }

  Array<Expr> to_array() const {
    Array<Expr> res = atomic_formulas;
    res.push_back(rest);
    return res;
  }
};

// The implementation of FactorOutAtomicFormulas
class FactorOutAtomicFormulasFunctor
  : public ExprFunctor<FactorOutAtomicFormulasResult(const Expr&, const Expr&)> {
 public:
  result_type Atomic_(const Expr& e) {
    // For atomic expressions the result is the expr itself with True as the residual
    return {{e}, make_const(e.type(), 1)};
  }

  // This is basically the list of expression kinds that are considered atomic
  result_type VisitExpr_(const Variable*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const Call*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const IntImm*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const UIntImm*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const EQ*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const NE*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const LE*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const LT*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const GE*, const Expr& e) final { return Atomic_(e); }
  result_type VisitExpr_(const GT*, const Expr& e) final { return Atomic_(e); }

  result_type VisitExpr_(const Select* op, const Expr& e) final {
    // Select can be rewritten through other logical ops
    Expr expr = (op->condition && op->true_value) || (!op->condition && op->false_value);
    return VisitExpr(expr, expr);
  }

  result_type VisitExpr_(const Not* op, const Expr& e) final {
    // Not should be moved down
    if (const Or* or_expr = op->a.as<Or>()) {
      Expr expr = !or_expr->a && !or_expr->b;
      return VisitExpr(expr, expr);
    } else if (const And* and_expr = op->a.as<And>()) {
      Expr expr = !and_expr->a || !and_expr->b;
      return VisitExpr(expr, expr);
    } if (const Select* sel_expr = op->a.as<Select>()) {
      Expr expr = ((!sel_expr->condition || !sel_expr->true_value) &&
                   (sel_expr->condition || !sel_expr->false_value));
      return VisitExpr(expr, expr);
    }
    return Atomic_(e);
  }

  result_type VisitExpr_(const And* op, const Expr& e) final {
    auto res_a = VisitExpr(op->a, op->a);
    auto res_b = VisitExpr(op->b, op->b);

    // For the And case we return the union of the sets of atomic formulas
    std::vector<Expr> res;
    res.reserve(res_a.atomic_formulas.size() + res_b.atomic_formulas.size());
    std::set_union(res_a.atomic_formulas.begin(), res_a.atomic_formulas.end(),
                   res_b.atomic_formulas.begin(), res_b.atomic_formulas.end(),
                   std::back_inserter(res),
                   ExprLess());

    // And the residuals are combined with &&
    return {res, res_a.rest && res_b.rest};
  }

  result_type VisitExpr_(const Mul* op, const Expr& e) final {
    // Since we work with bools, for multiplication we do the same thing as for And
    Expr e_and = op->a && op->b;
    return VisitExpr(e_and, e_and);
  }

  result_type VisitExpr_(const Or* op, const Expr& e) final {
    auto res_a = VisitExpr(op->a, op->a);
    auto res_b = VisitExpr(op->b, op->b);

    // For the Or case we intersect the sets of atomic formulas
    std::vector<Expr> res;
    res.reserve(std::min(res_a.atomic_formulas.size(), res_b.atomic_formulas.size()));
    std::set_intersection(res_a.atomic_formulas.begin(), res_a.atomic_formulas.end(),
                          res_b.atomic_formulas.begin(), res_b.atomic_formulas.end(),
                          std::back_inserter(res),
                          ExprLess());

    // Computing the residual is more complex: we have to compute the sets of atomic formulas
    // which are left behind, and then combine them with the residuals into the new residual.

    std::vector<Expr> new_cond_a;
    new_cond_a.reserve(res_a.atomic_formulas.size() - res.size());
    std::set_difference(res_a.atomic_formulas.begin(), res_a.atomic_formulas.end(),
                        res.begin(), res.end(),
                        std::back_inserter(new_cond_a),
                        ExprLess());

    std::vector<Expr> new_cond_b;
    new_cond_b.reserve(res_b.atomic_formulas.size() - res.size());
    std::set_difference(res_b.atomic_formulas.begin(), res_b.atomic_formulas.end(),
                        res.begin(), res.end(),
                        std::back_inserter(new_cond_b),
                        ExprLess());

    res_a.atomic_formulas = std::move(new_cond_a);
    res_b.atomic_formulas = std::move(new_cond_b);

    Expr new_rest = res_a.to_expr() || res_b.to_expr();

    return {res, new_rest};
  }
};

// Transform the given formula into a conjunction of atomic formulas (represented as an array)
// and a non-atomic residual. Atomic formulas are consts, calls, variables and comparisons (a <= b,
// etc), i.e. formulas which are not logical operators (||, &&, !) on the top level.
FactorOutAtomicFormulasResult FactorOutAtomicFormulas(const Expr& e) {
  CHECK(e.type().is_bool());
  return FactorOutAtomicFormulasFunctor().VisitExpr(e, e);
}


class RemoveRedundantInequalitiesMutator : public IRMutator {
 public:
  explicit RemoveRedundantInequalitiesMutator(Array<Expr> known) {
    for (const Expr& cond : known) {
      known_.push_back(SuperSimplify(cond));
    }
  }

  virtual Expr Mutate_(const Select* op, const Expr& e) {
    bool has_side_effect = HasSideEffect(e);
    Expr new_cond = SuperSimplify(Mutate(op->condition));
    if (is_one(new_cond) && !has_side_effect) {
      return Mutate(op->true_value);
    } else if (is_zero(new_cond) && !has_side_effect) {
      return Mutate(op->false_value);
    } else {
      Array<Expr> new_known = known_;
      for (const Expr& atomic : FactorOutAtomicFormulas(new_cond).atomic_formulas) {
        new_known.push_back(atomic);
      }
      RemoveRedundantInequalitiesMutator new_mutator(new_known);
      // Note that we mutate only the true value with the new mutator
      // TODO(sgrechanik-h): Update known conditions for the false value as well
      return Select::make(new_cond, new_mutator.Mutate(op->true_value), Mutate(op->false_value));
    }
  }

  virtual Expr Mutate_(const Call* op, const Expr& e) {
    if (op->name == intrinsic::tvm_if_then_else) {
      Expr new_cond = SuperSimplify(Mutate(op->args[0]));
      if (is_one(new_cond)) {
        return Mutate(op->args[1]);
      } else if (is_zero(new_cond)) {
        return Mutate(op->args[2]);
      } else {
        Array<Expr> new_known = known_;
        for (const Expr& atomic : FactorOutAtomicFormulas(new_cond).atomic_formulas) {
          new_known.push_back(atomic);
        }
        RemoveRedundantInequalitiesMutator new_mutator(new_known);
        // Note that we mutate only the true value with the new mutator
        // TODO(sgrechanik-h): Update known conditions for the false value as well
        return if_then_else(new_cond, new_mutator.Mutate(op->args[1]), Mutate(op->args[2]));
      }
    } else {
      return IRMutator::Mutate_(op, e);
    }
  }

  virtual Expr Mutate_(const Reduce* op, const Expr& e) {
    Array<Expr> known_with_axes = known_;
    for (const Expr& axis_cond : IterVarsToInequalities(op->axis)) {
      known_with_axes.push_back(axis_cond);
    }
    RemoveRedundantInequalitiesMutator mutator_with_axes(known_with_axes);

    Expr new_cond = mutator_with_axes.Mutate(op->condition);

    Array<Expr> new_known = known_with_axes;
    for (const Expr& atomic : FactorOutAtomicFormulas(new_cond).atomic_formulas) {
      new_known.push_back(atomic);
    }
    RemoveRedundantInequalitiesMutator new_mutator(new_known);

    Array<Expr> new_source;
    for (const Expr& src : op->source) {
      new_source.push_back(new_mutator.Mutate(src));
    }

    return Reduce::make(op->combiner, new_source, op->axis, new_cond, op->value_index);
  }

  virtual Expr Mutate_(const EQ* op, const Expr& e) { return MutateAtomic_(e); }
  virtual Expr Mutate_(const NE* op, const Expr& e) { return MutateAtomic_(e); }
  virtual Expr Mutate_(const LT* op, const Expr& e) { return MutateAtomic_(e); }
  virtual Expr Mutate_(const LE* op, const Expr& e) { return MutateAtomic_(e); }
  virtual Expr Mutate_(const GT* op, const Expr& e) { return MutateAtomic_(e); }
  virtual Expr Mutate_(const GE* op, const Expr& e) { return MutateAtomic_(e); }

  virtual Expr Mutate_(const And* op, const Expr& e) {
    return Mutate(op->a) && Mutate(op->b);
  }

 private:
  Expr MutateAtomic_(const Expr& e) {
    Expr simplified = SuperSimplify(e);
    for (const Expr& other : known_) {
      if (Equal(simplified, other)) {
        return const_true();
      }
    }
    return simplified;
  }

  Array<Expr> known_;
};

// Propagate information from conditions and remove redundant inequalities
// TODO(sgrechanik-h): This should be merged into standard simplifiers
Expr RemoveRedundantInequalities(const Expr& expr, const Array<Expr>& known) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);
  ZE_LOG_VAR(known);
  return ZE_LOG_RES(RemoveRedundantInequalitiesMutator(known).Mutate(expr));
}


struct EliminateDivModResult {
  Expr expr;
  Map<Var, Expr> substitution;
  Array<Var> new_variables;
  Array<Expr> conditions;
  Map<Var, Range> ranges;
};

// TODO: This is a duplicate of the same enum from canonical_simplify.cc, they should be merged
enum DivMode {
  /*! \brief Truncated division. */
  kTruncDiv,
  /*! \brief Floor division. */
  kFloorDiv
};

inline Expr ModImpl(Expr a, Expr b, DivMode mode) {
  if (mode == kTruncDiv) {
    return truncmod(a, b);
  } else {
    CHECK_EQ(mode, kFloorDiv);
    return floormod(a, b);
  }
}

inline Expr DivImpl(Expr a, Expr b, DivMode mode) {
  if (mode == kTruncDiv) {
    return truncdiv(a, b);
  } else {
    CHECK_EQ(mode, kFloorDiv);
    return floordiv(a, b);
  }
}

class EliminateDivModMutator : public IRMutator {
 public:
  Map<Var, Expr> substitution;
  Array<Var> new_variables;
  Array<Expr> conditions;
  Map<Var, Range> ranges;

  explicit EliminateDivModMutator(Map<Var, Range> ranges)
    : ranges(ranges) {}

  virtual Expr Mutate_(const Div* op, const Expr& e) {
    const IntImm* imm = op->b.as<IntImm>();
    if (imm && imm->value != 0) {
      if (imm->value < 0) {
        // x / -c == -(x/c) for truncated division
        return make_zero(op->type) - Mutate(truncdiv(op->a, make_const(op->type, -imm->value)));
      }

      // Try to find the already existing variables for this expression
      auto it = expr_to_vars_.find(std::make_tuple(kTruncDiv, op->a, imm->value));
      if (it != expr_to_vars_.end()) {
        return it->second.first;
      }

      // Otherwise recursively mutate the left hand side, and create new variables
      Expr mutated_a = Mutate(op->a);
      if (auto var_pair_opt = AddNewVarPair(op->a, mutated_a, imm->value, kTruncDiv)) {
        return var_pair_opt.value().first;
      } else {
        return truncdiv(mutated_a, op->b);
      }
    }

    return truncdiv(Mutate(op->a), Mutate(op->b));
  }

  virtual Expr Mutate_(const Mod* op, const Expr& e) {
    const IntImm* imm = op->b.as<IntImm>();
    if (imm && imm->value != 0) {
      if (imm->value < 0) {
        // x % -c == x % c for truncated division
        return Mutate(truncmod(op->a, make_const(op->type, -imm->value)));
      }

      // Try to find the already existing variables for this expression
      auto it = expr_to_vars_.find(std::make_tuple(kTruncDiv, op->a, imm->value));
      if (it != expr_to_vars_.end()) {
        return it->second.second;
      }

      // Otherwise recursively mutate the left hand side, and create new variables
      Expr mutated_a = Mutate(op->a);
      if (auto var_pair_opt = AddNewVarPair(op->a, mutated_a, imm->value, kTruncDiv)) {
        return var_pair_opt.value().second;
      } else {
        return truncmod(mutated_a, op->b);
      }
    }

    return truncmod(Mutate(op->a), Mutate(op->b));
  }

  virtual Expr Mutate_(const FloorDiv* op, const Expr& e) {
    const IntImm* imm = op->b.as<IntImm>();
    if (imm && imm->value != 0) {
      if (imm->value < 0) {
        // x / -c == (-x) / c for flooring division
        return Mutate(floordiv(make_zero(op->type) - op->a, make_const(op->type, -imm->value)));
      }

      // Try to find the already existing variables for this expression
      auto it = expr_to_vars_.find(std::make_tuple(kFloorDiv, op->a, imm->value));
      if (it != expr_to_vars_.end()) {
        return it->second.first;
      }

      // Otherwise recursively mutate the left hand side, and create new variables
      Expr mutated_a = Mutate(op->a);
      if (auto var_pair_opt = AddNewVarPair(op->a, mutated_a, imm->value, kFloorDiv)) {
        return var_pair_opt.value().first;
      } else {
        return floordiv(mutated_a, op->b);
      }
    }

    return floordiv(Mutate(op->a), Mutate(op->b));
  }

  virtual Expr Mutate_(const FloorMod* op, const Expr& e) {
    const IntImm* imm = op->b.as<IntImm>();
    if (imm && imm->value != 0) {
      if (imm->value < 0) {
        // x % -c == -(-x % c) for flooring division
        return Mutate(make_zero(op->type) - floormod(make_zero(op->type) - op->a,
                                                     make_const(op->type, -imm->value)));
      }

      // Try to find the already existing variables for this expression
      auto it = expr_to_vars_.find(std::make_tuple(kFloorDiv, op->a, imm->value));
      if (it != expr_to_vars_.end()) {
        return it->second.second;
      }

      // Otherwise recursively mutate the left hand side, and create new variables
      Expr mutated_a = Mutate(op->a);
      if (auto var_pair_opt = AddNewVarPair(op->a, mutated_a, imm->value, kFloorDiv)) {
        return var_pair_opt.value().second;
      } else {
        return floormod(mutated_a, op->b);
      }
    }

    return floormod(Mutate(op->a), Mutate(op->b));
  }

 private:
  dmlc::optional<std::pair<Var, Var>> AddNewVarPair(const Expr& e,
                                                    const Expr& mut,
                                                    int64_t val,
                                                    DivMode mode) {
    using tresult = dmlc::optional<std::pair<Var, Var>>;

    // Try to find the variables using the mutated expressions
    if (!e.same_as(mut)) {
      auto it = expr_to_vars_.find(std::make_tuple(mode, mut, val));
      if (it != expr_to_vars_.end()) {
        return tresult(it->second);
      }
    }

    Expr val_e = make_const(e.type(), val);
    idx_ += 1;

    // Convert `ranges` to IntSets
    std::unordered_map<const Variable*, IntSet> var_intsets;
    for (const auto& p : ranges) {
      var_intsets[p.first.get()] = IntSet::range(p.second);
    }

    // Infer ranges for the expressions we want to replace with variables
    Range div_range = EvalSet(DivImpl(mut, val_e, mode), var_intsets).cover_range(Range());
    Range mod_range = EvalSet(ModImpl(mut, val_e, mode), var_intsets).cover_range(Range());

    // We don't want to add unbounded variables
    if (!div_range.get() || !mod_range.get()) {
      LOG(WARNING) << "EliminateDivMod: won't eliminate " << DivImpl(e, val_e, mode)
                   << "  because its bounds cannot be inferred";
      return tresult();
    }
    if (!mod_range.get()) {
      LOG(WARNING) << "EliminateDivMod: won't eliminate " << ModImpl(e, val_e, mode)
                   << "  because its bounds cannot be inferred";
      return tresult();
    }

    // Create new variables for the expressions
    auto div = Var((mode == kTruncDiv ? "tdiv" : "fdiv") + std::to_string(idx_), e.type());
    auto mod = Var((mode == kTruncDiv ? "tmod" : "fmod") + std::to_string(idx_), e.type());

    new_variables.push_back(div);
    new_variables.push_back(mod);

    // Note that we have to perform substitution to mut because mut may contain new variables
    substitution.Set(div, DivImpl(Substitute(mut, substitution), val_e, mode));
    substitution.Set(mod, ModImpl(Substitute(mut, substitution), val_e, mode));

    ranges.Set(div, div_range);
    ranges.Set(mod, mod_range);

    // This additional condition works as a definition for the new variables
    conditions.push_back(mut == div*val_e + mod);

    if (!CanProve(mod_range->extent <= val_e)) {
      // Since we use the C/C++ definition of mod, there may be multiple values of `mod`
      // satisfying the added condition if the expr `e` may change its sign, so we
      // have to add another condition.
      LOG(WARNING) << "EliminateDivMod: cannot fully eliminate div or mod because "
                   << ModImpl(e, val_e, mode) << "  probably may change its sign";
      conditions.push_back(Select::make(e >= 0, mod >= 0, mod <= 0));
    }

    auto p = std::make_pair(div, mod);
    expr_to_vars_[std::make_tuple(mode, e, val)] = p;
    if (!e.same_as(mut)) {
      expr_to_vars_[std::make_tuple(mode, mut, val)] = p;
    }
    return tresult(p);
  }

  // A custom comparison function for pairs of exprs and numbers. Compares exprs deeply.
  struct Compare_ {
    bool operator()(const std::tuple<DivMode, Expr, int64_t>& p1,
                    const std::tuple<DivMode, Expr, int64_t>& p2) const {
      if (std::get<0>(p1) < std::get<0>(p2)) {
        return true;
      } else if (std::get<0>(p1) > std::get<0>(p2)) {
        return false;
      } else if (std::get<2>(p1) < std::get<2>(p2)) {
        return true;
      } else if (std::get<2>(p1) > std::get<2>(p2)) {
        return false;
      } else {
        return Compare(std::get<1>(p1), std::get<1>(p2)) < 0;
      }
    }
  };

  // A counter for naming new variables
  int idx_{0};
  // A map from pairs of exprs and numbers (e, n) to pairs of new vars (div, mod)
  // such that `div = e / n` and `mod = e % n`
  std::map<std::tuple<DivMode, Expr, int64_t>, std::pair<Var, Var>, Compare_>
    expr_to_vars_;
};

// Replace every subexpr of the form e/const and e % const with a new variable.
// Syntactically equal expressions will be mapped to the same variable.
EliminateDivModResult EliminateDivMod(const Expr& expr, Map<Var, Range> ranges) {
  EliminateDivModResult res;
  EliminateDivModMutator mutator(ranges);
  res.expr = mutator.Mutate(expr);
  res.conditions = std::move(mutator.conditions);
  res.new_variables = std::move(mutator.new_variables);
  res.substitution = std::move(mutator.substitution);
  res.ranges = std::move(mutator.ranges);
  return res;
}

// run EliminateDivMod from the conditions of a domain
DomainTransformation EliminateDivModFromDomainConditions(const Domain& domain) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(domain);

  auto elim_res = EliminateDivMod(All(domain->conditions), domain->ranges);

  Map<Var, Range> new_vranges = elim_res.ranges;
  Array<Var> new_axis = Concat(domain->variables, elim_res.new_variables);
  Expr new_cond = elim_res.expr && All(elim_res.conditions);

  Domain new_domain =
      DomainNode::make(new_axis, FactorOutAtomicFormulas(new_cond).to_array(), new_vranges);

  Map<Var, Expr> old_to_new;
  Map<Var, Expr> new_to_old = elim_res.substitution;
  for (const Var& v : domain->variables) {
    old_to_new.Set(v, v);
    new_to_old.Set(v, v);
  }

  return ZE_LOG_RES(DomainTransformationNode::make(new_domain, domain, new_to_old, old_to_new));
}

// run EliminateDivMod from the condition of a reduction
Expr EliminateDivModFromReductionCondition(const Expr& expr,
                                           Map<Var, Range> vranges = Map<Var, Range>()) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);
  ZE_LOG_VAR(vranges);

  if (const Reduce* red = expr.as<Reduce>()) {
    for (const IterVar& iv : red->axis) {
      vranges.Set(iv->var, iv->dom);
    }

    auto elim_res = EliminateDivMod(red->condition, vranges);

    vranges = elim_res.ranges;

    Array<IterVar> new_axis =
        Concat(red->axis, IterVarsFromMap(elim_res.new_variables, vranges, kCommReduce));

    Expr new_cond = elim_res.expr && All(elim_res.conditions);

    return ZE_LOG_RES(Reduce::make(red->combiner, red->source, new_axis,
                                   new_cond, red->value_index));
  } else {
    return ZE_LOG_RES(expr);
  }
}

// Add copies of outer variables (that are used in the conditions but are missing from the domain
// variables) to the list of domain variables.
DomainTransformation AddOuterVariablesIntoDomain(const Domain& domain) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(domain);

  std::unordered_set<const Variable*> vset;
  for (const Var& v : domain->variables) {
    vset.insert(v.get());
  }

  Array<Var> new_variables = domain->variables;
  Map<Var, Expr> outer_to_new;
  Map<Var, Expr> new_to_old;
  Array<Expr> new_conditions;
  Map<Var, Range> new_ranges = domain->ranges;
  for (const Expr& cond : domain->conditions) {
    for (const Var& v : ExprFreeVars(cond)) {
      if (!vset.count(v.get())) {
        Var new_var = v.copy_with_suffix("Z");
        new_variables.push_back(new_var);
        outer_to_new.Set(v, new_var);
        new_to_old.Set(new_var, v);
        if (domain->ranges.count(v)) {
          new_ranges.Set(new_var, domain->ranges[v]);
        }
        vset.insert(new_var.get());
        vset.insert(v.get());
        new_conditions.push_back(new_var == v);
      }
    }
    new_conditions.push_back(Substitute(cond, outer_to_new));
  }

  Map<Var, Expr> old_to_new;
  for (const Var& v : domain->variables) {
    old_to_new.Set(v, v);
    new_to_old.Set(v, v);
  }

  Domain new_domain = DomainNode::make(new_variables, new_conditions, new_ranges);
  return ZE_LOG_RES(DomainTransformationNode::make(new_domain, domain, new_to_old, old_to_new));
}


std::tuple<int64_t, int64_t, int64_t> xgcd(int64_t a, int64_t b) {
  int64_t s = 0, old_s = 1;
  int64_t t = 1, old_t = 0;
  int64_t r = b, old_r = a;

  while (r != 0) {
    int64_t q = old_r / r;
    std::swap(r, old_r);
    r -= q * old_r;
    std::swap(s, old_s);
    s -= q * old_s;
    std::swap(t, old_t);
    t -= q * old_t;
  }

  CHECK_EQ(a % old_r, 0);
  CHECK_EQ(b % old_r, 0);
  CHECK(old_r == old_s*a + old_t*b);

  return std::make_tuple(old_r, old_s, old_t);
}

DomainTransformation SolveSystemOfEquations(const Domain& domain) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(domain);

  // Conditions we don't know what to do with
  std::vector<Expr> rest;
  // Matrix represented as a vector of rows, each row is an array of coefficients
  std::vector<std::vector<int64_t>> matrix;
  // A column of right hand sides
  std::vector<Expr> rhs;
  // A map from old vars to new vars represented as a matrix, each row of this matrix corresponds to
  // an old variable (from domain->variables) and represents a vector of coefficients
  std::vector<std::vector<int64_t>> old_to_new;
  // A map from new vars to old vars represented directly as an array of expressions
  std::vector<Expr> new_to_old;

  // auto dumpall = [&]() {
  //   std::cout << "Matrix:\n";
  //   for (size_t i = 0; i < matrix.size(); ++i) {
  //     for (auto e : matrix[i]) {
  //       std::cout << e << "\t";
  //     }
  //     std::cout << "\t->\t" << rhs[i];
  //     std::cout << "\n";
  //   }
  //   std::cout << "old_to_new:\n";
  //   for (const auto& r : old_to_new) {
  //     for (auto e : r) {
  //       std::cout << e << "\t";
  //     }
  //     std::cout << "\n";
  //   }
  //   std::cout << "new_to_old:\n" << Array<Expr>(new_to_old);
  //   std::cout << "\n" << std::endl;
  // };

  size_t vars_size = domain->variables.size();

  // Initialize the old_to_new matrix with the identity matrix
  for (size_t i = 0; i < vars_size; ++i) {
    old_to_new.emplace_back(vars_size);
    old_to_new.back()[i] = 1;
    new_to_old.push_back(domain->variables[i]);
  }

  // Transform formulas into rows of the matrix
  for (const Expr& formula : domain->conditions) {
    if (const EQ* eq = formula.as<EQ>()) {
      Array<Expr> coefs = arith::DetectLinearEquation(SuperSimplify(eq->a - eq->b, domain->ranges),
                                                      domain->variables);
      if (!coefs.empty()) {
        std::vector<int64_t> row;
        for (size_t j = 0; j < coefs.size() - 1; ++j) {
          Expr c = coefs[j];
          if (const IntImm* intimm = c.as<IntImm>()) {
            row.push_back(intimm->value);
          } else {
            row.clear();
            break;
          }
        }

        if (!row.empty()) {
          matrix.push_back(row);
          rhs.push_back(-coefs[coefs.size() - 1]);
          continue;
        }
      }
    }

    // otherwise
    rest.push_back(formula);
  }

  // Diagonalize the matrix
  for (size_t index = 0; index < std::min(matrix.size(), vars_size); ++index) {
    // Here the matrix is partially diagonalized, that is matrix[i, j] is zero for all i, j
    // such that (i < index) or (j < index), unless (i == j).
    // That is, now we are diagonalizing the submatrix with i >= index and j >= index

    // Find a row with a nonzero element in the index-th column
    // (We also prefer rows where this element has minimal abs value)
    size_t best_i = index;
    for (auto i = best_i; i < matrix.size(); ++i) {
      int64_t m_old = matrix[best_i][index];
      int64_t m_new = matrix[i][index];
      if (m_new != 0) {
        if (m_old == 0 || std::abs(m_new) < std::abs(m_old)) {
          best_i = i;
        }
      }
    }
    // Move the row we found to the index-th position
    std::swap(matrix[index], matrix[best_i]);
    std::swap(rhs[index], rhs[best_i]);

    // If the index-th diagonal element is still zero, try to find a column with nonzero index-th
    // element and move it to the index-th position
    if (matrix[index][index] == 0) {
      for (size_t j = index + 1; j < vars_size; ++j) {
        if (matrix[index][j] != 0) {
          for (size_t i = index; i < matrix.size(); ++i) {
            std::swap(matrix[i][index], matrix[i][j]);
          }
          // swapping columns corresponds to swapping the corresponding new variables
          std::swap(new_to_old[index], new_to_old[j]);
          for (size_t i = 0; i < old_to_new.size(); ++i) {
            std::swap(old_to_new[i][index], old_to_new[i][j]);
          }
          break;
        }
      }
    }

    // If the index-th diagonal element is still zero, then both the index-th row and the index-th
    // column are completely zero, and we don't need to do anything; just go to the next index
    if (matrix[index][index] == 0) {
      continue;
    }

    // Now the index-th diagonal element is non-zero and we can zero all the index-th column
    // below it by subtracting rows from each other
    for (auto i = index + 1; i < matrix.size(); ++i) {
      if (matrix[i][index] != 0) {
        int64_t g, a, b;
        // g = a*matrix[index][index] + b*matrix[i][index]
        if (matrix[i][index] % matrix[index][index] != 0) {
          std::tie(g, a, b) = xgcd(matrix[index][index], matrix[i][index]);
        } else {
          // Explicitly avoid changing the index-th row. This is important to avoid infinite
          // loop.
          g = matrix[index][index];
          a = 1;
          b = 0;
        }

        // Let m = matrix[index][index], n = matrix[i][index], then the following is true:
        //
        // [ a   n/g ][ m/g  n/g ] = [ 1  0 ]
        // [ b  -m/g ][ b    -a  ] = [ 0  1 ]
        //
        // Note that the two matrices are integer (since g = gcd(m, n)).
        // We will essentially multiply our matrix on the left by a dilated and transposed version
        // of the first of these two matrices. The second matrix is not needed here, however we will
        // use it while zeroing the index-th row.

        int64_t m_g = matrix[index][index] / g;
        int64_t n_g = matrix[i][index] / g;

        // Note that j is the index of the column, not the row
        for (size_t j = index; j < matrix[i].size(); ++j) {
          // Multiply index-th row by a and add the i-th row multiplied by b
          // This will make the index-th diagonal element equal to the gcd
          int64_t new_index_j = a*matrix[index][j] + b*matrix[i][j];
          // This transformation performs zeroing of matrix[i][index]
          int64_t new_i_j = n_g*matrix[index][j] - m_g*matrix[i][j];
          matrix[index][j] = new_index_j;
          matrix[i][j] = new_i_j;
        }
        // We have to do the same with rhs
        Expr ea = make_const(rhs[index].type(), a);
        Expr eb = make_const(rhs[i].type(), b);
        Expr e_m_g = make_const(rhs[i].type(), m_g);
        Expr e_n_g = make_const(rhs[index].type(), n_g);
        Expr new_index_rhs = ea*rhs[index] + eb*rhs[i];
        Expr new_i_rhs = e_n_g*rhs[index] - e_m_g*rhs[i];
        rhs[index] = new_index_rhs;
        rhs[i] = new_i_rhs;
      }
    }

    bool changed = false;

    // Now we have to zero the elements of the index-th row by manipulating columns.
    // This is more difficult because column manipulation corresponds to variable manipulation,
    // but the algorithm is essentially the same as before.
    for (size_t j = index + 1; j < vars_size; ++j) {
      if (matrix[index][j] != 0) {
        int64_t g, a, b;
        // g = a*matrix[index][index] + b*matrix[index][j]
        if (matrix[index][j] % matrix[index][index] != 0) {
          std::tie(g, a, b) = xgcd(matrix[index][index], matrix[index][j]);
          // During this phase we may disrupt the zeroness of the index-th column, so we will
          // have to take some action if this might have happened.
          changed = true;
        } else {
          // Explicitly avoid changing the index-th column. This is important to avoid infinite
          // loop. Note that here we don't have to set `changed` to true since we don't change the
          // index-th column.
          g = matrix[index][index];
          a = 1;
          b = 0;
        }

        // Let m = matrix[index][index], n = matrix[index][j], then the following is true:
        //
        // [ a   n/g ][ m/g  n/g ] = [ 1  0 ]
        // [ b  -m/g ][ b    -a  ] = [ 0  1 ]
        //
        // Now we are going to multiply our matrix on the right (to manipulate columns instead of
        // rows), we will also transform the old_to_new matrix the same way, and we will use the
        // second matrix to transform new_to_old.

        int64_t m_g = matrix[index][index] / g;
        int64_t n_g = matrix[index][j] / g;

        for (size_t i = index; i < matrix.size(); ++i) {
          int64_t new_i_index = a*matrix[i][index] + b*matrix[i][j];
          int64_t new_i_j = n_g*matrix[i][index] - m_g*matrix[i][j];
          matrix[i][index] = new_i_index;
          matrix[i][j] = new_i_j;
        }
        // We do exactly the same transformations with old_to_new
        for (size_t i = 0; i < old_to_new.size(); ++i) {
          int64_t new_i_index = a*old_to_new[i][index] + b*old_to_new[i][j];
          int64_t new_i_j = n_g*old_to_new[i][index] - m_g*old_to_new[i][j];
          old_to_new[i][index] = new_i_index;
          old_to_new[i][j] = new_i_j;
        }
        // And apply reverse transformations to new_to_old.
        Expr ea = make_const(new_to_old[j].type(), a);
        Expr eb = make_const(new_to_old[index].type(), b);
        Expr e_m_g = make_const(new_to_old[index].type(), m_g);
        Expr e_n_g = make_const(new_to_old[j].type(), n_g);
        Expr new_index = e_m_g*new_to_old[index] + e_n_g*new_to_old[j];
        Expr new_j = eb*new_to_old[index] - ea*new_to_old[j];
        new_to_old[index] = new_index;
        new_to_old[j] = new_j;
      }
    }

    if (changed) {
      // We might have changed the first column, so we have to zero it once more (or at least check
      // if it's zero), so just perform this iteration once more.
      index -= 1;
    }
  }

  Array<Var> new_vars;
  Map<Var, Expr> new_to_old_map;
  std::vector<Expr> solution;
  Array<Expr> conditions;

  // Simplify right hand sides
  for (Expr& r : rhs) {
    r = SuperSimplify(r, domain->ranges);
  }

  // Create the conditions of the existence of a solution
  for (size_t j = 0; j < matrix.size(); ++j) {
    Expr new_cond;
    if (j >= vars_size || matrix[j][j] == 0) {
      // The row of matrix is zero. A solution exists only if the rhs[j] is also zero
      new_cond = (rhs[j] == 0);
    } else {
      // The diagonal element is non-zero. A solution exists only if the diagonal element
      // is a divisor of the rhs[j]
      new_cond = (floormod(rhs[j], std::abs(matrix[j][j])) == 0);
    }
    new_cond = SuperSimplify(new_cond, domain->ranges);
    if (is_const_int(new_cond, 0)) {
      return ZE_LOG_RES(EmptyDomainTransformation(domain));
    } else if (!is_const_int(new_cond, 1)) {
      conditions.push_back(new_cond);
    }
  }

  // Now create new variables or directly solve the equations
  for (size_t j = 0; j < vars_size; ++j) {
    if (j >= matrix.size() || matrix[j][j] == 0) {
      // The j-th variable can take any integer value, create a tvm variable for it
      Expr to_old = SuperSimplify(new_to_old[j], domain->ranges);
      std::string name_hint = "n" + std::to_string(new_vars.size());
      if (const Variable* v_old = to_old.as<Variable>()) {
        name_hint += "_" + v_old->name_hint;
      }
      Var v = Var(name_hint, new_to_old[j].type());
      solution.push_back(v);
      new_vars.push_back(v);
      new_to_old_map.Set(v, to_old);
    } else {
      // The j-th variable is just a single value, don't create a tvm variable
      if (matrix[j][j] >= 0) {
        Expr a = make_const(rhs[j].type(), matrix[j][j]);
        solution.push_back(SuperSimplify(floordiv(rhs[j], a), domain->ranges));
      } else {
        // This is required because some simplifiers have problems with dividing by negative numbers
        Expr a = make_const(rhs[j].type(), -matrix[j][j]);
        solution.push_back(SuperSimplify(floordiv(-rhs[j], a), domain->ranges));
      }
    }
  }

  // Convert the old_to_new matrix to map
  Map<Var, Expr> old_to_new_map;
  for (size_t i = 0; i < vars_size; ++i) {
    Expr e = make_zero(domain->variables[i].type());
    for (size_t j = 0; j < vars_size; ++j) {
      e = e + make_const(e.type(), old_to_new[i][j])*solution[j];
    }
    e = SuperSimplify(e);
    old_to_new_map.Set(domain->variables[i], e);
  }

  // From now on we will use sorted domain variable ranges to increase determinism
  std::vector<std::pair<Var, Range>> sorted_domain_ranges = VarMapToVectorOfPairs(domain->ranges);

  // The resulting ranges
  Map<Var, Range> ranges;

  // First of all, fill the new ranges with outer variable ranges
  std::unordered_set<const Variable*> vset;
  for (const Var& v : domain->variables) {
    vset.insert(v.get());
  }
  for (const auto& p : sorted_domain_ranges) {
    if (!vset.count(p.first.get())) {
      ranges.Set(p.first, p.second);
    }
  }

  // Convert original ranges to IntSets
  std::unordered_map<const Variable*, IntSet> var_intsets;
  for (const auto& p : sorted_domain_ranges) {
    var_intsets[p.first.get()] = IntSet::range(p.second);
  }

  // Infer ranges for the new variables and add them to the resulting ranges
  for (const auto& p : new_to_old_map) {
    Range range = EvalSet(p.second, var_intsets).cover_range(Range());
    if (range.defined()) {
      ranges.Set(p.first, range);
    }
  }

  // We have to transform ranges of the old variables into conditions over new variables because new
  // ranges are not enough usually.
  for (const auto& p : sorted_domain_ranges) {
    if (old_to_new_map.count(p.first)) {
      Expr in_terms_of_new = old_to_new_map[p.first];
      Expr lower_cond = SuperSimplify(p.second->min <= in_terms_of_new, ranges);
      Expr upper_cond = SuperSimplify(in_terms_of_new < p.second->min + p.second->extent, ranges);
      if (!is_const_int(lower_cond, 1)) {
        conditions.push_back(lower_cond);
      }
      if (!is_const_int(upper_cond, 1)) {
        conditions.push_back(upper_cond);
      }
    }
  }

  // Add the rest conditions
  for (const Expr& cond : rest) {
    conditions.push_back(Substitute(cond, old_to_new_map));
  }

  Domain new_domain = DomainNode::make(new_vars, conditions, ranges);
  return ZE_LOG_RES(DomainTransformationNode::make(new_domain, domain,
                                                   new_to_old_map, old_to_new_map));
}


VarBounds VarBounds::substitute(const Map<Var, Expr>& subst) const {
  auto apply_fun = [&subst](const Expr& e) { return Substitute(e, subst); };
  return {Substitute(coef, subst),
          UpdateArray(lower, apply_fun),
          UpdateArray(equal, apply_fun),
          UpdateArray(upper, apply_fun)};
}

Array<Expr> SolveSystemOfInequalitiesResult::as_conditions() const {
  Array<Expr> res;
  for (const Var& v : variables) {
    auto it = bounds.find(v.get());
    CHECK(it != bounds.end());
    const VarBounds& bnds = it->second;
    Expr lhs = bnds.coef * v;
    for (const Expr& rhs : bnds.equal) {
      res.push_back(EQ::make(lhs, rhs));
    }
    for (const Expr& rhs : bnds.lower) {
      res.push_back(GE::make(lhs, rhs));
    }
    for (const Expr& rhs : bnds.upper) {
      res.push_back(LE::make(lhs, rhs));
    }
  }
  for (const Expr& e : other_conditions) {
    res.push_back(e);
  }
  return res;
}

// Rewrite the system of inequalities using Fourier-Motzkin elimination
// Note that variable ranges help a lot, so this parameter is even non-optional
SolveSystemOfInequalitiesResult SolveSystemOfInequalities(const Array<Expr>& inequalities,
                                                          const Array<Var>& variables,
                                                          const Map<Var, Range>& vranges) {
  SolveSystemOfInequalitiesResult res;
  res.variables = variables;

  // The algorithm consists in doing the following things for each variable v
  // - Take formulas from `current` and classify them according to polarity wrt v
  // - Combine each formula of positive polarity (wrt v) with each formula of negative polarity
  // - Put the resulting combinations into `new_current` along with unclassifiable formulas
  // - Replace `current` with `new_current` and move to the next variable

  // current and new_current are sorted to enable some heuristics
  std::set<Expr, ExprLess> current;
  std::set<Expr, ExprLess> new_current;
  // A vector of pairs (c, e), c > 0, representing formulas of the form c*v + e <= 0
  std::vector<std::pair<int64_t, Expr>> coef_pos;
  // A vector of pairs (c, e), c < 0, representing formulas of the form c*v + e <= 0
  std::vector<std::pair<int64_t, Expr>> coef_neg;

  // formulas we don't know what to do with
  std::vector<Expr> rest;

  // A helper that adds an inequality to new_current if it's not obviously redundant
  auto add_to_new_current = [&new_current, &vranges] (const Expr& new_ineq) {
    if (CanProve(new_ineq, vranges)) {
      // redundant: follows from the vranges
      return;
    }
    if (const LE* new_le = new_ineq.as<LE>()) {
        // A heuristic: check if the new inequality is a consequence of one
        // of its future neighbors (in this case don't add it) or if a future neighbor is
        // a consequence of the new ineq (in which case remove the neighbor)
        auto it_neighbor = new_current.lower_bound(new_ineq);
        if (it_neighbor != new_current.begin()) {
          const LE* le = std::prev(it_neighbor)->as<LE>();
          if (le && CanProve(new_le->a - le->a <= 0, vranges)) {
            return;
          } else if (le && CanProve(le->a - new_le->a <= 0, vranges)) {
            new_current.erase(std::prev(it_neighbor));
          }
        }
        // Check the other neighbor
        if (it_neighbor != new_current.end()) {
          const LE* le = it_neighbor->as<LE>();
          if (le && CanProve(new_le->a - le->a <= 0, vranges)) {
            return;
          } else if (le && CanProve(le->a - new_le->a <= 0, vranges)) {
            it_neighbor = new_current.erase(it_neighbor);
          }
        }

        new_current.insert(it_neighbor, new_ineq);
    } else {
      new_current.insert(new_ineq);
    }
  };

  // Simplify each inequality into the form `expr <= 0` and add to new_current formulas
  for (const Expr& ineq : inequalities) {
    add_to_new_current(NormalizeComparisons(SuperSimplify(ineq, vranges)));
  }

  std::swap(current, new_current);

  for (const Var& v : variables) {
    CHECK(!res.bounds.count(v.get())) <<
      "Variable " << v << " appears several times in the `variables` which might be a bug";

    new_current.clear();
    coef_pos.clear();
    coef_neg.clear();

    // Add bounds from vranges
    if (vranges.count(v)) {
      const Range& range = vranges[v];
      Expr range_lbound = SuperSimplify(range->min, vranges);
      Expr range_ubound = SuperSimplify(range->min + range->extent - 1, vranges);
      coef_neg.push_back({-1, range_lbound});
      coef_pos.push_back({1, -range_ubound});
    }

    // Take formulas from `current` and classify them according to polarity wrt v
    for (const Expr& ineq : current) {
      if (const LE* le = ineq.as<LE>()) {
        Array<Expr> coef = arith::DetectLinearEquation(le->a, {v});
        if (!coef.empty() && is_const(coef[0])) {
          int64_t coef0 = *as_const_int(coef[0]);
          if (coef0 == 0) {
            // zero polarity, straight to new_current
            add_to_new_current(ineq);
          } else if (coef0 > 0) {
            coef_pos.push_back({coef0, coef[1]});
          } else if (coef0 < 0) {
            coef_neg.push_back({coef0, coef[1]});
          }
          continue;
        }
      } else if (const EQ* eq = ineq.as<EQ>()) {
        Array<Expr> coef = arith::DetectLinearEquation(eq->a, {v});
        if (!coef.empty() && is_const(coef[0])) {
          int64_t coef0 = *as_const_int(coef[0]);
          if (coef0 == 0) {
            // zero polarity, straight to new_current
            add_to_new_current(ineq);
          } else if (coef0 > 0) {
            // Equalities may be considered as pairs of two inequalities
            coef_pos.push_back({coef0, coef[1]});
            coef_neg.push_back({-coef0, -coef[1]});
          } else if (coef0 < 0) {
            coef_pos.push_back({-coef0, -coef[1]});
            coef_neg.push_back({coef0, coef[1]});
          }
          continue;
        }
      }

      // if nothing worked, put it in rest
      rest.push_back(ineq);
    }

    // Combine each positive inequality with each negative one (by adding them together)
    for (const auto& pos : coef_pos) {
      for (const auto& neg : coef_neg) {
        auto first_gcd = gcd(pos.first, -neg.first);
        Expr c_pos = make_const(v.type(), neg.first/first_gcd);
        Expr c_neg = make_const(v.type(), pos.first/first_gcd);
        Expr new_lhs = c_neg*neg.second - c_pos*pos.second;
        Expr new_ineq = LE::make(new_lhs, make_zero(pos.second.type()));
        new_ineq = NormalizeComparisons(SuperSimplify(new_ineq, vranges));
        add_to_new_current(new_ineq);
      }
    }

    // Now we have to generate resulting (in)equalities for the variable v

    // Find the common denominator in a sense
    // We will generate formulas of the form coef_lcm*v <= bound
    int64_t coef_lcm = 1;
    for (const auto& pos : coef_pos) {
      coef_lcm = lcm(coef_lcm, pos.first);
    }
    for (const auto& neg : coef_neg) {
      coef_lcm = lcm(coef_lcm, -neg.first);
    }

    // The resulting lower and upper bounds stored in sorted vectors
    std::vector<Expr> upper_bounds;
    std::vector<Expr> lower_bounds;
    upper_bounds.reserve(coef_pos.size());
    lower_bounds.reserve(coef_neg.size());

    for (const auto& pos : coef_pos) {
      Expr bound = make_const(v.type(), -coef_lcm/pos.first)*pos.second;
      bound = SuperSimplify(bound, vranges);
      // Don't add if any of the existing bounds is better
      if (std::any_of(upper_bounds.begin(), upper_bounds.end(),
                      [&bound, &vranges](const Expr& o) { return CanProve(o - bound <= 0,
                                                                          vranges); })) {
        continue;
      }
      // Erase all worse bounds
      upper_bounds.erase(
        std::remove_if(upper_bounds.begin(), upper_bounds.end(),
                       [&bound, &vranges](const Expr& o) { return CanProve(o - bound >= 0,
                                                                           vranges); }),
        upper_bounds.end());
      // Add
      upper_bounds.push_back(bound);
    }
    for (const auto& neg : coef_neg) {
      Expr bound = make_const(v.type(), -coef_lcm/neg.first)*neg.second;
      bound = SuperSimplify(bound, vranges);
      // Don't add if any of the existing bounds is better
      if (std::any_of(lower_bounds.begin(), lower_bounds.end(),
                      [&bound, &vranges](const Expr& o) { return CanProve(o - bound >= 0,
                                                                          vranges); })) {
        continue;
      }
      // Erase all worse bounds
      lower_bounds.erase(
        std::remove_if(lower_bounds.begin(), lower_bounds.end(),
                       [&bound, &vranges](const Expr& o) { return CanProve(o - bound <= 0,
                                                                           vranges); }),
        lower_bounds.end());
      // Add
      lower_bounds.push_back(bound);
    }

    // Sort the vectors and remove duplicates
    for (std::vector<Expr>* bounds : {&upper_bounds, &lower_bounds}) {
      std::sort(bounds->begin(), bounds->end(), ExprLess());
      bounds->erase(std::unique(bounds->begin(), bounds->end(), ExprEq()), bounds->end());
    }

    // Bounds which are both lower and upper should go to equal...
    std::vector<Expr> equal;
    equal.reserve(std::min(upper_bounds.size(), lower_bounds.size()));
    std::set_intersection(upper_bounds.begin(), upper_bounds.end(),
                          lower_bounds.begin(), lower_bounds.end(),
                          std::back_inserter(equal), ExprLess());

    // ...and be removed from upper bounds...
    std::vector<Expr> new_upper;
    new_upper.reserve(upper_bounds.size() - equal.size());
    std::set_difference(upper_bounds.begin(), upper_bounds.end(),
                        equal.begin(), equal.end(),
                        std::back_inserter(new_upper), ExprLess());

    // ...and from lower bounds.
    std::vector<Expr> new_lower;
    new_lower.reserve(lower_bounds.size() - equal.size());
    std::set_difference(lower_bounds.begin(), lower_bounds.end(),
                        equal.begin(), equal.end(),
                        std::back_inserter(new_lower), ExprLess());

    // Write it to the result.
    auto& bnds = res.bounds[v.get()];
    bnds.coef = make_const(v.type(), coef_lcm);
    bnds.equal = equal;
    bnds.lower = new_lower;
    bnds.upper = new_upper;

    std::swap(current, new_current);
  }

  // Everything that is left goes to res.other_conditions
  for (const Expr& e : current) {
    Expr e_simp = SuperSimplify(e, vranges);
    if (is_const_int(e_simp, 0)) {
      // contradiction detected
      res.other_conditions = {const_false()};
      return res;
    } else if (is_const_int(e_simp, 1)) {
      continue;
    } else {
      res.other_conditions.push_back(e_simp);
    }
  }

  for (const Expr& e : rest)
    res.other_conditions.push_back(e);

  return res;
}


// Deskew the given domain
DomainTransformation DeskewDomain(const Domain& domain) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(domain);

  // Resulting ranges will contain ranges for the new variables and for the variables that are
  // not in the domain->variables but are in domain->ranges
  Map<Var, Range> res_ranges;

  // vars are variables from domain's variables followed by all the other variables from its ranges
  Array<Var> vars = domain->variables;
  for (const auto& pair : VarMapToVectorOfPairs(domain->ranges)) {
    bool already = false;
    for (const Var& v : vars) {
      already = already || v.same_as(pair.first);
    }
    if (!already) {
      vars.push_back(pair.first);
      // Also populate the resulting ranges with ranges of outer variables
      res_ranges.Set(pair.first, pair.second);
    }
  }

  auto solved_system = SolveSystemOfInequalities(domain->conditions, vars, domain->ranges);

  ZE_LOG("Conds after FME", solved_system.as_conditions());

  Map<Var, Expr> res_old_to_new;
  Map<Var, Expr> res_new_to_old;
  Array<Var> res_variables;
  Array<Expr> res_conditions;
  std::unordered_map<const Variable*, IntSet> new_var_intsets;

  Map<Var, Range> vranges = domain->ranges;

  // Initialize new_var_intsets with the old var intsets
  for (const auto& pair : domain->ranges) {
    new_var_intsets[pair.first.get()] = IntSet::range(pair.second);
  }

  // We process variables in the reverse direction to start with the most independent one.
  // This order is needed to compute new ranges.
  for (auto it = domain->variables.rbegin(); it != domain->variables.rend(); ++it) {
    const Var& var = *it;
    ZE_LOG_NL();
    ZE_LOG("Processing variable", var);
    auto& bnd = solved_system.bounds[var.get()];
    // Note that we replace old vars with new ones
    bnd = bnd.substitute(res_old_to_new);
    ZE_LOG("Coefficient", bnd.coef);
    if (is_one(bnd.coef) && !bnd.equal.empty()) {
      // There is an equation of the form `v == expr`, so this variable can be completely removed.
      // Note that we use the 0-th expression because they are ordered by complexity, so it must be
      // the simplest one.
      res_old_to_new.Set(var, bnd.equal[0]);
      ZE_LOG("Replaced with", bnd.equal[0]);
    } else {
      std::vector<Expr> lowers(bnd.equal.begin(), bnd.equal.end());
      std::vector<Expr> uppers(bnd.equal.begin(), bnd.equal.end());
      for (const auto& expr : bnd.lower) lowers.push_back(expr);
      for (const auto& expr : bnd.upper) uppers.push_back(expr);

      ZE_LOG("LowersUnsorted", Array<Expr>(lowers));
      ZE_LOG("UppersUnsorted", Array<Expr>(uppers));

      std::sort(lowers.begin(), lowers.end(), ExprLess());
      std::sort(uppers.begin(), uppers.end(), ExprLess());

      ZE_LOG("Lowers", Array<Expr>(lowers));
      ZE_LOG("Uppers", Array<Expr>(uppers));

      // Here we will try all pairs of lower and upper bounds and find the best pair, that is, the
      // pair with the minimal difference between the upper and the lower.
      // Note that the bounds are for v, not for v*coef, because we will need bounds for v anyway

      // The lower bound of the best pair so far
      Expr best_lower = vranges[var]->min;
      // The difference between the upper and the lower of the best pair, maybe overapproximation
      Expr best_diff_over = vranges[var]->extent - 1;

      ZE_LOG("Initial best low", best_lower);
      ZE_LOG("Initial best diff_over", best_diff_over);

      for (const Expr& low : lowers) {
        for (const Expr& upp : uppers) {
          ZE_LOG_NL();
          ZE_LOG("Considering low", low);
          ZE_LOG("Considering upp", upp);
          Expr diff_1 = SuperSimplify(floordiv(upp - low, bnd.coef), vranges);
          // Since diff may depend on some other variables, we compute its overapproximation
          Expr diff_over_1 = SuperSimplify(EvalSet(diff_1, new_var_intsets).max(), vranges);

          // low is the lower bound for v*coef, but we need the lower bound for v.
          // We use rounding-up division to compute it. Since we want to use a single formula
          Expr low_divided = SuperSimplify(floordiv(low + bnd.coef - 1, bnd.coef), vranges);

          ZE_LOG("Considering low_divided", low_divided);
          ZE_LOG("Considering diff_1", diff_1);
          ZE_LOG("Considering diff_over_1", diff_over_1);

          // Compute another difference which may be more precise (or not).
          Expr diff_2 = SuperSimplify(floordiv(upp, bnd.coef) - low_divided, vranges);
          Expr diff_over_2 = SuperSimplify(EvalSet(diff_2, new_var_intsets).max(), vranges);

          ZE_LOG("Considering diff_2", diff_2);
          ZE_LOG("Considering diff_over_2", diff_over_2);

          if (CanProve(diff_over_2 - diff_over_1 < 0)) {
            diff_over_1 = diff_over_2;
          }

          Expr diff_over_1_is_better_expr;
          diff_over_1_is_better_expr = diff_over_1 - best_diff_over < 0;

          // If it is provable that the new one is strictly better than the current best one,
          // then replace it. Note that we are biased towards earlier pairs which should be simpler.
          if (CanProve(diff_over_1_is_better_expr, vranges)) {
            ZE_LOG("Current best low", low_divided);
            ZE_LOG("Current best diff", diff_over_1);
            best_lower = low_divided;
            best_diff_over = diff_over_1;
          }
        }
      }

      ZE_LOG_NL();
      ZE_LOG("Resulting best low", best_lower);
      ZE_LOG("Resulting best diff_over", best_diff_over);

      std::string suffix = Equal(best_lower, vranges[var]->min) ? "" : ".shifted";
      Var new_var = var.copy_with_suffix(suffix);

      Expr diff = SuperSimplify(best_diff_over, vranges);

      if (is_const_int(diff, 0)) {
        // Don't create an itervar, just replace it everywhere with its min
        res_old_to_new.Set(var, best_lower);
        ZE_LOG("Replaced with", best_lower);
      } else {
        res_old_to_new.Set(var, new_var + best_lower);
        // Note that we are substituting old with new, so best_lower contains new var,
        // that is we have to substitute new with old in best_lower here
        res_new_to_old.Set(new_var,
                           SuperSimplify(var - Substitute(best_lower, res_new_to_old), vranges));

        new_var_intsets[new_var.get()] = IntSet::interval(make_zero(new_var.type()), diff);

        // Add the new var to the resulting axis
        auto range = Range(make_zero(new_var.type()), SuperSimplify(diff + 1, vranges));
        res_variables.push_back(new_var);
        res_ranges.Set(new_var, range);
        vranges.Set(new_var, range);

        ZE_LOG("Replaced with", new_var + best_lower);
        ZE_LOG("New var range", range);
      }
    }
  }

  // Add the original conditions (with variables substituted) to the resulting conditions
  for (const Expr& old_cond : solved_system.as_conditions()) {
    Expr new_cond = SuperSimplify(Substitute(old_cond, res_old_to_new), vranges);
    if (!is_const_int(new_cond, 1)) {
      res_conditions.push_back(new_cond);
    }
  }

  // Reverse the axis so that it matches the order of the original variables
  res_variables = Array<Var>(res_variables.rbegin(), res_variables.rend());

  Domain new_domain = DomainNode::make(res_variables, res_conditions, res_ranges);
  return ZE_LOG_RES(DomainTransformationNode::make(new_domain, domain,
                                                   res_new_to_old, res_old_to_new));
}


// Simplify an iteration domain.
DomainTransformation SimplifyDomain(const Domain& domain,
                                    bool eliminate_div_mod) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(domain);
  ZE_LOG_VAR(eliminate_div_mod);

  DomainTransformation transf = IdDomainTransformation(domain);

  if (eliminate_div_mod) {
    transf += EliminateDivModFromDomainConditions(transf->new_domain);
  }

  // TODO(sgrechanik-h): Repeating the following steps has a positive effect, however we probably
  // should find a better terminating criterion (like stop when the domain volume stops decreasing)
  // Also 2 steps seems to be slightly better than 3
  for (size_t i = 0; i < 2; ++i) {
    DomainTransformation tr;
    tr = SolveSystemOfEquations(transf->new_domain);
    transf += tr;
    // TODO(sgrechanik-h): This helps for some artificial examples, however I'm not sure about
    // enabling it in general. The problem it solves is propagating equalities of outer vars.
    // tr = AddOuterVariablesIntoDomain(transf->new_domain);
    // transf += tr;
    tr = DeskewDomain(transf->new_domain);
    transf += tr;
  }

  return ZE_LOG_RES(transf);
}

// Use the condition of a reduction op to simplify its domain (axis)
Expr SimplifyReductionDomain(const Expr& expr, const Map<Var, Range>& outer_vranges) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);
  ZE_LOG_VAR(outer_vranges);

  if (const Reduce* red = expr.as<Reduce>()) {
    Map<Var, Range> vranges = Merge(outer_vranges, IterVarsToMap(red->axis));
    Domain domain = DomainNode::make(IterVarsToVars(red->axis),
                                     FactorOutAtomicFormulas(red->condition).to_array(),
                                     Merge(outer_vranges, IterVarsToMap(red->axis)));
    auto res = SimplifyDomain(domain);

    Array<Expr> new_source;
    for (const Expr& src : red->source) {
      new_source.push_back(Substitute(src, res->old_to_new));
    }

    Array<IterVar> new_axis =
        IterVarsFromMap(res->new_domain->variables, res->new_domain->ranges, kCommReduce);

    // Perform simplification mainly to remove a possibly empty reduction.
    return ZE_LOG_RES(SuperSimplify(Reduce::make(red->combiner, new_source, new_axis,
                                                 All(res->new_domain->conditions),
                                                 red->value_index)));
  } else {
    return ZE_LOG_RES(expr);
  }
}

// Extract the given expr under the given condition as a separate tensor if the volume of the
// extracted tensor will be less than the volume of the outer_axis
Expr ExtractAsTensorMaybe(const Expr& expr, const Expr& cond,
                          const Array<Var>& outer_axis,
                          const Map<Var, Range>& vranges) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);
  ZE_LOG_VAR(cond);
  ZE_LOG_VAR(outer_axis);
  ZE_LOG_VAR(vranges);

  Domain domain = DomainNode::make(outer_axis,
                                   FactorOutAtomicFormulas(cond).to_array(),
                                   vranges);
  auto res = SimplifyDomain(domain);

  Expr new_expr = SuperSimplify(Substitute(expr, res->old_to_new), res->new_domain->ranges);
  // This is mostly done to simplify if_then_else which is not known by the Halide simplifier
  new_expr = RemoveRedundantInequalities(new_expr, res->new_domain->conditions);

  // Keep only those variables of the new vars which are used in the new_expr
  Array<Var> used_res_variables;
  for (const Var& var : res->new_domain->variables) {
    if (ExprUseVar(new_expr, var)) {
      used_res_variables.push_back(var);
    }
  }

  // If the expression does not use vars then it is probably better to keep it inlined
  if (used_res_variables.empty()) {
    // We can return the new_expr here instead of the old expr because it doesn't use variables
    // otherwise we would need to replace the new vars or create a let-expression
    return ZE_LOG_RES(new_expr);
  }

  // If it's already a call to a tensor then extracting it will probably be useless
  if (const Call* call = new_expr.as<Call>()) {
    if (call->call_type == Call::CallType::Halide) {
      return ZE_LOG_RES(expr);
    }
  }

  // Compute volumes before and after
  Expr old_volume = make_const(Int(64), 1);
  for (const Var& var : outer_axis) {
    old_volume = old_volume * vranges[var]->extent;
  }

  Expr new_volume = make_const(Int(64), 1);
  for (const Var& var : used_res_variables) {
    new_volume = new_volume * res->new_domain->ranges[var]->extent;
  }

  // if we can prove that the old volume is not greater than the new volume then
  // prefer the old expression.
  if (CanProve(old_volume <= new_volume, vranges)) {
    return ZE_LOG_RES(expr);
  }

  Tensor tensor =
    op::TensorFromExpr(new_expr, IterVarsFromMap(used_res_variables, res->new_domain->ranges),
                       "extracted_tensor");

  Array<Expr> args;
  for (const Var& var : used_res_variables) {
    args.push_back(res->new_to_old[var]);
  }

  return ZE_LOG_RES(Call::make(expr.type(), tensor->op->name, args,
                               Call::CallType::Halide, tensor->op, tensor->value_index));
}


// Extract from cond an implication of cond not containing vars
std::pair<Expr, Expr> ImplicationNotContainingVars(
    const Expr& cond, const std::unordered_set<const Variable*>& vars) {
  CHECK(cond.type().is_bool()) << "The type of cond must be bool";
  // TODO(sgrechanik-h): not
  if (const And* op = cond.as<And>()) {
    auto pair_a = ImplicationNotContainingVars(op->a, vars);
    auto pair_b = ImplicationNotContainingVars(op->b, vars);
    return {pair_a.first && pair_b.first,
            pair_a.second && pair_b.second};
  } else if (const Or* op = cond.as<Or>()) {
    auto pair_a = ImplicationNotContainingVars(op->a, vars);
    auto pair_b = ImplicationNotContainingVars(op->b, vars);
    return {pair_a.first || pair_b.first,
            (pair_a.first || pair_b.second) &&
            (pair_b.first || pair_a.second) &&
            (pair_a.second || pair_b.second)};
  } else if (!ExprUseVar(cond, vars)) {
    return {cond, const_true()};
  } else {
    return {const_true(), cond};
  }
}

// Factor conditions out of a reduction by applying Fourier-Motzkin elimination and moving out
// (in)equalities which do not depend on the reduction variables.
std::pair<Expr, Expr> LiftConditionsThroughReduction(const Expr& cond,
                                                     const Array<IterVar>& red_axis,
                                                     const Array<IterVar>& outer_axis) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(cond);
  ZE_LOG_VAR(red_axis);
  ZE_LOG_VAR(outer_axis);

  // Factor out atomics so that we can consider this as a system of inequalities
  auto factoratomic_res = FactorOutAtomicFormulas(cond);
  Array<Expr> atomics = factoratomic_res.atomic_formulas;
  const Expr& rest = factoratomic_res.rest;

  Array<Var> allvars;
  for (const IterVar& v : red_axis) {
    allvars.push_back(v->var);
  }
  for (const IterVar& v : outer_axis) {
    allvars.push_back(v->var);
  }

  auto vranges = Merge(IterVarsToMap(red_axis), IterVarsToMap(outer_axis));
  // start from reduction vars, so that input vars don't depend on them
  atomics = SolveSystemOfInequalities(atomics, allvars, vranges).as_conditions();

  // Append the rest part
  Expr rewritten_cond = All(atomics) && rest;

  std::unordered_set<const Variable*> vset;
  for (const IterVar& v : red_axis) {
    vset.insert(v->var.get());
  }

  // The outer (first) condition does not contain reduction vars,
  // the inner (second) condition is everything else
  auto res = ImplicationNotContainingVars(rewritten_cond, vset);
  ZE_LOG_VAR(res.first);
  ZE_LOG_VAR(res.second);
  return res;
}

class ExtractReductionsMutator : public IRMutator {
 public:
  explicit ExtractReductionsMutator(const Array<Var>& outer_axis,
                                    Map<Var, Range> vranges,
                                    std::string name = "extracted_reduction")
    : outer_axis_(outer_axis), vranges_(std::move(vranges)), name_(std::move(name)) {}

  Expr Mutate_(const Reduce* op, const Expr& e) {
    ExtractReductionsMutator new_mutator(Concat(IterVarsToVars(op->axis), outer_axis_),
                                         Merge(vranges_, IterVarsToMap(op->axis)),
                                         name_);

    Array<Expr> new_source;
    for (const Expr& src : op->source) {
      new_source.push_back(new_mutator.Mutate(src));
    }

    Expr new_reduce =
      Reduce::make(op->combiner, new_source, op->axis, op->condition, op->value_index);

    ExprFreeVarsVisitor fv_visitor;
    fv_visitor.Visit(new_reduce);

    // Vars of the tensor we are going to create for this reduction
    Array<Var> vars;
    for (const Var& v : outer_axis_) {
      // We take variables from the outer_axis_ which are also present in the new reduction
      if (fv_visitor.free.count(v.get())) {
        vars.push_back(v);
      }
    }

    auto newaxis_vmap_pair = CloneIterVars(IterVarsFromMap(vars, vranges_));
    Array<IterVar> new_axis = newaxis_vmap_pair.first;
    new_reduce = SuperSimplify(Substitute(new_reduce, newaxis_vmap_pair.second),
                               IterVarsToMap(new_axis));

    Tensor tensor = op::TensorFromExpr(new_reduce, new_axis, name_, tag_, attrs_);

    Array<Expr> args;
    for (const Var& v : vars) {
      args.push_back(v);
    }

    return Call::make(e.type(), tensor->op->name, args,
                      Call::CallType::Halide, tensor->op, tensor->value_index);
  }

 private:
  Array<Var> outer_axis_;
  Map<Var, Range> vranges_;
  std::string name_;
  std::string tag_;
  Map<std::string, NodeRef> attrs_;
};

// Extract reductions as separate tensors.
Expr ExtractReductions(const Expr& expr,
                       const Array<Var>& outer_axis,
                       const Map<Var, Range>& vranges) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);
  ZE_LOG_VAR(outer_axis);
  ZE_LOG_VAR(vranges);
  return ZE_LOG_RES(ExtractReductionsMutator(outer_axis, vranges).Mutate(expr));
}

Expr ExtractNonTopReductions(const Expr& expr,
                             const Array<Var>& outer_axis,
                             const Map<Var, Range>& vranges) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr);
  ZE_LOG_VAR(outer_axis);
  ZE_LOG_VAR(vranges);

  if (const Reduce* red = expr.as<Reduce>()) {
    Array<Var> new_outer_axis = Concat(IterVarsToVars(red->axis), outer_axis);
    Map<Var, Range> new_vranges = Merge(vranges, IterVarsToMap(red->axis));
    Array<Expr> new_source;
    for (const Expr& src : red->source) {
      new_source.push_back(ExtractReductions(src, new_outer_axis, new_vranges));
    }
    Expr new_condition = ExtractReductions(red->condition, new_outer_axis, new_vranges);

    return ZE_LOG_RES(Reduce::make(red->combiner, new_source, red->axis,
                                   new_condition, red->value_index));
  } else {
    return ZE_LOG_RES(ExtractReductions(expr, outer_axis, vranges));
  }
}

Expr OptimizeAndLiftNonzeronessConditionsImpl(const Expr& expr_orig,
                                              const Array<IterVar>& axis,
                                              const Map<Var, Range>& vranges) {
  ZE_LOG_ENTER();
  ZE_LOG_VAR(expr_orig);
  ZE_LOG_VAR(axis);
  ZE_LOG_VAR(vranges);

  Expr result;
  Map<Var, Range> combined_vranges = Merge(vranges, IterVarsToMap(axis));

  // Simplify the original expression first, mostly to simplify combiners
  Expr expr = SuperSimplify(expr_orig, combined_vranges);
  ZE_LOG("expr (after simplification)", expr);

  if (const Reduce* red = expr.as<Reduce>()) {
    // TODO(sgrechanik-h): There are some other operations which behave like sum
    bool is_sum = IsSumCombiner(red->combiner, vranges);
    if (is_sum || CanFactorZeroFromCombiner(red->combiner, red->value_index, vranges)) {
      Expr new_red = expr;

      // Here we simplify the reduction
      {
        Expr cond = red->condition;
        Array<Expr> source = red->source;

        // If it is a summation then we can lift nonzeroness conditions from the source
        // and add them to the reduction conditions
        if (is_sum) {
          auto nz = NonzeronessCondition(red->source[red->value_index]);
          cond = nz.cond && cond;
          source.Set(0, nz.value);
        }

        new_red = Reduce::make(red->combiner, source, red->axis, cond, red->value_index);
        new_red = SimplifyReductionDomain(new_red, combined_vranges);
        red = new_red.as<Reduce>();

        // If the reduction disappears completely then transform the result as a non-reduction
        if (!red) {
          return ZE_LOG_RES(OptimizeAndLiftNonzeronessConditionsImpl(new_red, axis, vranges));
        }
      }

      Expr new_outer_cond, new_reduce_cond;
      Array<Expr> new_source = red->source;

      // Partially lift conditions from the reduce condition
      std::tie(new_outer_cond, new_reduce_cond) =
        LiftConditionsThroughReduction(red->condition, red->axis, axis);

      // If it's not sum then we haven't yet lifted nonzeroness cond from the source
      if (!is_sum) {
        Expr outer_nz_cond, nz_cond, nz_source;
        auto nz = NonzeronessCondition(red->source[red->value_index]);
        // Append conditions from the reduction
        nz_cond = new_reduce_cond && nz.cond;
        nz_source = nz.value;
        std::tie(outer_nz_cond, nz_cond) =
          LiftConditionsThroughReduction(nz_cond, red->axis, axis);
        new_outer_cond = new_outer_cond && outer_nz_cond;
        new_source.Set(red->value_index, SelectElseZero(nz_cond, nz_source));
      }

      Expr new_reduce = Reduce::make(red->combiner, new_source, red->axis,
                                     new_reduce_cond, red->value_index);
      new_reduce = ExtractAsTensorMaybe(new_reduce, new_outer_cond,
                                        IterVarsToVars(axis),
                                        combined_vranges);
      result = SelectElseZero(new_outer_cond, new_reduce);
    } else {
      return ZE_LOG_RES(SimplifyReductionDomain(expr, combined_vranges));
    }
  } else {
    auto nz = NonzeronessCondition(expr);
    Expr new_expr = ExtractAsTensorMaybe(nz.value, nz.cond,
                                         IterVarsToVars(axis),
                                         combined_vranges);
    result = SelectElseZero(nz.cond, new_expr);
  }

  // Note that RemoveRedundantInequalities can sometimes propagate equalities which
  // other simplifiers cannot, like (i % 3) == 0.
  Array<Expr> axis_conds = IterVarsToInequalities(axis);
  result = RemoveRedundantInequalities(result, axis_conds);

  // Sometimes ExtractAsTensorMaybe doesn't perform extraction, so there may be some non-top
  // reductions left, take care of them
  result = SuperSimplify(ExtractNonTopReductions(result, IterVarsToVars(axis), combined_vranges),
                         combined_vranges);
  return ZE_LOG_RES(result);
}

Tensor OptimizeAndLiftNonzeronessConditions(const Tensor& tensor, const Map<Var, Range>& vranges) {
  auto transform_func = [&vranges](const Expr& expr, const Array<IterVar>& axis) {
    return OptimizeAndLiftNonzeronessConditionsImpl(expr, axis, vranges);
  };
  return op::TransformBody(tensor, transform_func);
}


Domain DomainNode::make(Array<Var> variables,
                        Array<Expr> conditions,
                        Map<Var, Range> ranges) {
  auto n = make_node<DomainNode>();
  n->variables = std::move(variables);
  n->conditions = std::move(conditions);
  n->ranges = std::move(ranges);
  return Domain(n);
}

TVM_STATIC_IR_FUNCTOR(IRPrinter, vtable)
.set_dispatch<DomainNode>([](const ObjectRef& ref, IRPrinter* p) {
    auto* d = static_cast<const DomainNode*>(ref.get());
    Type type = d->variables.empty() ? Int(32) : d->ranges[d->variables[0]]->extent.type();
    Expr volume = make_const(type, 1);
    for (const auto& v : d->variables) {
      if (d->ranges.count(v)) {
        volume *= d->ranges[v]->extent;
      } else {
        volume = Expr();
        return;
      }
    }
    if (volume.get()) {
      p->stream << "Domain(box_volume=" << volume;
    } else {
      p->stream << "Domain(box_volume=inf";
    }
    p->stream << ", variables=" << d->variables
              << ", conditions=" << d->conditions
              << ", ranges=" << PrintSortedVarMap(d->ranges) << ")";
  });

TVM_REGISTER_NODE_TYPE(DomainNode);


DomainTransformation DomainTransformationNode::make(Domain new_domain,
                                                    Domain old_domain,
                                                    Map<Var, Expr> new_to_old,
                                                    Map<Var, Expr> old_to_new) {
  auto n = make_node<DomainTransformationNode>();
  n->new_domain = std::move(new_domain);
  n->old_domain = std::move(old_domain);
  n->new_to_old = std::move(new_to_old);
  n->old_to_new = std::move(old_to_new);
  return DomainTransformation(n);
}

TVM_STATIC_IR_FUNCTOR(IRPrinter, vtable)
.set_dispatch<DomainTransformationNode>([](const ObjectRef& ref, IRPrinter* p) {
    auto* d = static_cast<const DomainTransformationNode*>(ref.get());
    p->stream << "DomainTransformation(new_domain=" << d->new_domain
              << ", old_domain=" << d->old_domain
              << ", new_to_old=" << PrintSortedVarMap(d->new_to_old)
              << ", old_to_new=" << PrintSortedVarMap(d->old_to_new) << ')';
  });

TVM_REGISTER_NODE_TYPE(DomainTransformationNode);


TVM_REGISTER_API("arith._make_Domain")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    if (args[1].IsObjectRef<Expr>()) {
      *ret = DomainNode::make(args[0], FactorOutAtomicFormulas(args[1]).to_array(), args[2]);
    } else {
      *ret = DomainNode::make(args[0], args[1], args[2]);
    }
  });

TVM_REGISTER_API("ir_pass.ComposeDomainTransformations")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = ComposeDomainTransformations(args[0], args[1]);
  });

TVM_REGISTER_API("ir_pass.EmptyDomainTransformation")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = EmptyDomainTransformation(args[0]);
  });

TVM_REGISTER_API("ir_pass.IdDomainTransformation")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = IdDomainTransformation(args[0]);
  });

TVM_REGISTER_API("ir_pass.SolveSystemOfEquations")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = SolveSystemOfEquations(args[0]);
  });

TVM_REGISTER_API("ir_pass.IsSumCombiner")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    if (args.size() >= 2) {
      *ret = IsSumCombiner(args[0], args[1]);
    } else {
      *ret = IsSumCombiner(args[0]);
    }
  });

TVM_REGISTER_API("ir_pass.CanFactorZeroFromCombiner")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    if (args.size() >= 3) {
      *ret = CanFactorZeroFromCombiner(args[0], args[1], args[2]);
    } else {
      *ret = CanFactorZeroFromCombiner(args[0], args[1]);
    }
  });

TVM_REGISTER_API("ir_pass.LiftNonzeronessCondition")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = LiftNonzeronessCondition(args[0]);
  });

TVM_REGISTER_API("ir_pass.InlineTailCall")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = InlineTailCall(args[0]);
  });

TVM_REGISTER_API("ir_pass.InlineTensors")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    if (args[0].IsObjectRef<Expr>()) {
      Expr e = args[0];
      if (args.size() == 1) {
        *ret = InlineTensors(e);
      } else if (args.size() == 2) {
        *ret = InlineTensors(e, args[1]);
      } else if (args.size() >= 3) {
        *ret = InlineTensors(e, args[1], args[2]);
      }
    } else if (args[0].IsObjectRef<Tensor>()) {
      Tensor t = args[0];
      if (args.size() == 1) {
        *ret = InlineTensors(t);
      } else if (args.size() == 2) {
        *ret = InlineTensors(t, args[1]);
      } else if (args.size() >= 3) {
        *ret = InlineTensors(t, args[1], args[2]);
      }
    }
  });

TVM_REGISTER_API("ir_pass.SolveSystemOfInequalities")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = SolveSystemOfInequalities(args[0], args[1], args[2]).as_conditions();
  });

TVM_REGISTER_API("ir_pass.SimplifyDomain")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    if (args.size() == 1) {
      *ret = SimplifyDomain(args[0]);
    } else {
      *ret = SimplifyDomain(args[0], args[1]);
    }
  });

TVM_REGISTER_API("ir_pass.SimplifyReductionDomain")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = SimplifyReductionDomain(args[0], args[1]);
  });

TVM_REGISTER_API("ir_pass.ExtractAsTensorMaybe")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = ExtractAsTensorMaybe(args[0], args[1], args[2], args[3]);
  });

TVM_REGISTER_API("ir_pass.ExtractReductions")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = ExtractReductions(args[0], args[1], args[2]);
  });

TVM_REGISTER_API("ir_pass.ExtractNonTopReductions")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    *ret = ExtractNonTopReductions(args[0], args[1], args[2]);
  });

TVM_REGISTER_API("ir_pass.OptimizeAndLiftNonzeronessConditions")
.set_body([](TVMArgs args, TVMRetValue *ret) {
    if (args.size() >= 2) {
      *ret = OptimizeAndLiftNonzeronessConditions(args[0], args[1]);
    } else {
      *ret = OptimizeAndLiftNonzeronessConditions(args[0]);
    }
  });

}  // namespace ir
}  // namespace tvm
