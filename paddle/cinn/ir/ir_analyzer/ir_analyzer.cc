// Copyright (c) 2023 CINN Authors. All Rights Reserved.
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

#include "paddle/cinn/ir/ir_analyzer/ir_analyzer.h"

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "paddle/cinn/common/context.h"
#include "paddle/cinn/common/integer_set.h"
#include "paddle/cinn/ir/ir_mutator.h"
#include "paddle/cinn/ir/ir_printer.h"
#include "paddle/cinn/ir/ir_visitor.h"
#include "paddle/cinn/ir/schedule/ir_schedule.h"
#include "paddle/cinn/ir/schedule/ir_schedule_util.h"
#include "paddle/cinn/ir/schedule/schedule_base.h"
#include "paddle/cinn/ir/schedule/schedule_desc.h"
#include "paddle/cinn/ir/tensor.h"
#include "paddle/cinn/ir/utils/ir_nodes_collector.h"
#include "paddle/cinn/utils/random_engine.h"
#include "paddle/common/enforce.h"
#include "paddle/fluid/platform/enforce.h"
namespace cinn {
namespace ir {
namespace analyzer {

bool HasBlock(const std::vector<Expr>& exprs, const std::string& block_name) {
  for (auto& it_expr : exprs) {
    FindBlocksVisitor visitor(block_name);
    auto find_blocks = visitor(&it_expr);
    if (!find_blocks.empty()) {
      PADDLE_ENFORCE_EQ(find_blocks.size(),
                        1U,
                        ::common::errors::InvalidArgument(
                            "There should not be more than 1 block with "
                            "identical name!"));
      return true;
    }
  }
  return false;
}

std::vector<Expr> GetLoops(const std::vector<Expr>& exprs,
                           const std::string& block_name) {
  Expr block = GetBlock(exprs, block_name);
  std::vector<Expr> result = GetLoops(exprs, block);
  return result;
}

std::vector<Expr> GetLoops(const std::vector<Expr>& exprs, const Expr& block) {
  std::vector<Expr> result;
  PADDLE_ENFORCE_NOT_NULL(
      block.As<ir::ScheduleBlockRealize>(),
      ::common::errors::InvalidArgument(
          "The block must be convertible to ir::ScheduleBlockRealize."));
  PADDLE_ENFORCE_NOT_NULL(
      block.As<ir::ScheduleBlockRealize>()
          ->schedule_block.As<ir::ScheduleBlock>(),
      ::common::errors::InvalidArgument(
          "Cannot cast block to ir::ScheduleBlockRealize."));
  std::string block_name = block.As<ir::ScheduleBlockRealize>()
                               ->schedule_block.As<ir::ScheduleBlock>()
                               ->name;

  for (auto& it_expr : exprs) {
    FindLoopsVisitor visitor(block);
    auto find_loops = visitor(&it_expr);
    if (!find_loops.empty()) {
      if (!result.empty()) {
        std::stringstream ss;
        ss << "Find block with name: \n"
           << block_name << " appeared in more than one AST!";
        PADDLE_THROW(::common::errors::InvalidArgument(ss.str()));
      }
      result = find_loops;
    }
  }

  return result;
}

std::vector<Expr> GetAllBlocks(const std::vector<Expr>& exprs) {
  std::vector<Expr> result;
  for (auto& it_expr : exprs) {
    FindBlocksVisitor visitor;
    auto find_blocks = visitor(&it_expr);
    result.insert(result.end(), find_blocks.begin(), find_blocks.end());
  }
  PADDLE_ENFORCE_EQ(
      result.empty(),
      false,
      ::common::errors::InvalidArgument("Didn't find blocks in expr."));
  return result;
}

std::vector<Expr> GetChildBlocks(const Expr& expr) {
  if (!expr.As<ir::ScheduleBlockRealize>()) {
    PADDLE_ENFORCE_NOT_NULL(expr.As<ir::For>(),
                            ::common::errors::InvalidArgument(
                                "The expression must be convertible to either "
                                "ir::ScheduleBlockRealize or ir::For."));
  }
  FindBlocksVisitor visitor;
  std::vector<Expr> result = visitor(&expr);
  return result;
}

Expr GetBlock(const std::vector<Expr>& exprs, const std::string& block_name) {
  Expr result;
  for (auto& it_expr : exprs) {
    FindBlocksVisitor visitor(block_name);
    auto find_blocks = visitor(&it_expr);
    if (!find_blocks.empty()) {
      PADDLE_ENFORCE_EQ(find_blocks.size(),
                        1U,
                        ::common::errors::InvalidArgument(
                            "There should not be more than 1 block with "
                            "identical name!"));
      result = find_blocks[0];
      return result;
    }
  }
  std::stringstream ss;
  ss << "Didn't find a block with name " << block_name
     << " in this ModuleExpr!";
  PADDLE_THROW(::common::errors::InvalidArgument(ss.str()));
}

Expr GetRootSBlock(const Expr& expr) {
  PADDLE_ENFORCE_NOT_NULL(expr.As<ir::Block>(),
                          ::common::errors::InvalidArgument(
                              "The root of ModuleExpr must be a Block."));
  PADDLE_ENFORCE_EQ(expr.As<ir::Block>()->stmts.size(),
                    1U,
                    ::common::errors::InvalidArgument(
                        "The root block must have exactly one stmt."));
  auto& root = expr.As<ir::Block>()->stmts[0];
  PADDLE_ENFORCE_NOT_NULL(
      root.As<ir::ScheduleBlockRealize>(),
      ::common::errors::InvalidArgument(
          "The first stmt in the block must be ScheduleBlockRealize."));
  return root;
}

Expr GetRootBlock(const std::vector<Expr>& exprs, const Expr& expr) {
  for (auto& it_expr : exprs) {
    auto find_expr = ir::ir_utils::CollectIRNodesWithoutTensor(
        it_expr,
        [&](const Expr* x) {
          return x->node_type() == expr.node_type() && *x == expr;
        },
        true);
    if (!find_expr.empty()) {
      return GetRootSBlock(it_expr);
    }
  }
  std::stringstream ss;
  ss << "Didn't find expr \n" << expr << "in StScheduleImpl:\n" << exprs[0];
  PADDLE_THROW(::common::errors::InvalidArgument(ss.str()));
}

DeviceAPI GetDeviceAPI(const std::vector<Expr>& exprs) {
  auto find_for_nodes = ir::ir_utils::CollectIRNodesWithoutTensor(
      exprs.front(), [&](const Expr* x) { return x->As<ir::For>(); }, true);
  PADDLE_ENFORCE_EQ(
      find_for_nodes.empty(),
      false,
      ::common::errors::InvalidArgument(
          "The find_for_nodes container is empty. It must not be empty."));
  return (*find_for_nodes.begin()).As<ir::For>()->device_api;
}

Expr AddUnitLoop(const std::vector<Expr>& exprs, const Expr& block) {
  PADDLE_ENFORCE_NOT_NULL(
      block.As<ir::ScheduleBlockRealize>(),
      ::common::errors::InvalidArgument(
          "The block is not convertible to ir::ScheduleBlockRealize. It must "
          "be convertible to ir::ScheduleBlockRealize."));
  PADDLE_ENFORCE_NOT_NULL(
      block.As<ir::ScheduleBlockRealize>()
          ->schedule_block.As<ir::ScheduleBlock>(),
      ::common::errors::InvalidArgument(
          "The schedule_block must be convertible to ir::ScheduleBlock."));
  std::string block_name = block.As<ir::ScheduleBlockRealize>()
                               ->schedule_block.As<ir::ScheduleBlock>()
                               ->name;

  FindBlockParent visitor(block_name);
  for (auto expr : exprs) {
    visitor(&expr);
    if (visitor.target_) {
      break;
    }
  }

  PADDLE_ENFORCE_NOT_NULL(visitor.target_,
                          ::common::errors::InvalidArgument(
                              "The visitor target is nullptr. It must not be "
                              "nullptr."));
  if (visitor.target_->As<ir::Block>()) {
    for (auto& stmt : visitor.target_->As<ir::Block>()->stmts) {
      if (stmt.As<ir::ScheduleBlockRealize>()) {
        if (stmt.As<ir::ScheduleBlockRealize>()
                ->schedule_block.As<ir::ScheduleBlock>()
                ->name == block_name) {
          auto block = ir::Block::Make({GetBlock(exprs, block_name)});
          auto loop = ir::For::Make(ir::Var(cinn::common::UniqName("ix")),
                                    ir::Expr(0),
                                    ir::Expr(1),
                                    ir::ForType::Serial,
                                    ir::DeviceAPI::UNK,
                                    block);
          stmt = loop;
          return loop;
        }
      }
    }
  } else if (visitor.target_->As<ir::For>()) {
    auto block = ir::Block::Make({visitor.target_->As<ir::For>()->body});
    auto loop = ir::For::Make(ir::Var(cinn::common::UniqName("ix")),
                              ir::Expr(0),
                              ir::Expr(1),
                              ir::ForType::Serial,
                              ir::DeviceAPI::UNK,
                              block);
    visitor.target_->As<ir::For>()->body = loop;
    return loop;
  } else if (visitor.target_->As<ir::ScheduleBlock>()) {
    auto block =
        ir::Block::Make({visitor.target_->As<ir::ScheduleBlock>()->body});
    auto loop = ir::For::Make(ir::Var(cinn::common::UniqName("ix")),
                              ir::Expr(0),
                              ir::Expr(1),
                              ir::ForType::Serial,
                              ir::DeviceAPI::UNK,
                              block);
    visitor.target_->As<ir::ScheduleBlock>()->body = loop;
    return loop;
  } else {
    PADDLE_THROW(
        ::common::errors::InvalidArgument("Can't find block's parent!"));
  }
  PADDLE_THROW(::common::errors::InvalidArgument(
      "Shouldn't reach code here in AddUnitLoop"));
  return Expr{nullptr};
}

Expr GetStoreOfSBlock(const Expr& block) {
  PADDLE_ENFORCE_NOT_NULL(block.As<ScheduleBlockRealize>(),
                          ::common::errors::InvalidArgument(
                              "Failed to cast block to ScheduleBlockRealize."));
  std::vector<Expr> find_store = ir_utils::CollectIRNodesWithoutTensor(
      block, [&](const Expr* x) { return x->As<Store>(); }, true);
  PADDLE_ENFORCE_EQ(find_store.size(),
                    1U,
                    ::common::errors::InvalidArgument(
                        "One block should only have one Store node!"));
  return *find_store.begin();
}

Tensor GetStoreTensorOfSBlock(const Expr& block) {
  PADDLE_ENFORCE_NOT_NULL(block.As<ScheduleBlockRealize>(),
                          ::common::errors::InvalidArgument(
                              "Failed to cast block to ScheduleBlockRealize."));
  Expr find_store = GetStoreOfSBlock(block);
  PADDLE_ENFORCE_NOT_NULL(
      find_store.As<Store>()->tensor.as_tensor(),
      ::common::errors::InvalidArgument(
          "The tensor must be convertible to Tensor type."));
  return find_store.As<Store>()->tensor.as_tensor_ref();
}

std::vector<Expr> GetConsumerSBlocks(const Expr& block, const Expr& root) {
  PADDLE_ENFORCE_NOT_NULL(block.As<ScheduleBlockRealize>(),
                          ::common::errors::InvalidArgument(
                              "Failed to cast block to ScheduleBlockRealize."));
  PADDLE_ENFORCE_NOT_NULL(
      root.As<ScheduleBlockRealize>(),
      ::common::errors::InvalidArgument(
          "Failed to cast 'root' to ScheduleBlockRealize."));
  std::vector<Expr> consumers;
  std::string store_tensor_name = GetStoreTensorOfSBlock(block)->name;
  if (IsReduceInitTensorName(store_tensor_name)) {
    std::string consumer_name = GetOriginalReduceTensorName(store_tensor_name);
    auto consumer =
        ir_utils::CollectIRNodesWithoutTensor(root, [&](const Expr* x) {
          return x->As<ScheduleBlockRealize>() &&
                 x->As<ScheduleBlockRealize>()
                         ->schedule_block.As<ScheduleBlock>()
                         ->name == consumer_name;
        });
    PADDLE_ENFORCE_EQ(consumer.size(),
                      1,
                      ::common::errors::InvalidArgument(
                          "The reduce tensor should have only one consumer!"));
    return {*consumer.begin()};
  }

  auto find_blocks =
      ir_utils::CollectIRNodesWithoutTensor(root, [&](const Expr* x) {
        return x->As<ScheduleBlockRealize>() && *x != block && *x != root;
      });
  for (auto& find_block : find_blocks) {
    PADDLE_ENFORCE_NOT_NULL(
        find_block.As<ScheduleBlockRealize>()
            ->schedule_block.As<ScheduleBlock>(),
        ::common::errors::InvalidArgument(
            "The schedule_block within ScheduleBlockRealize must be "
            "convertible to ScheduleBlock type."));
    auto block_body = find_block.As<ScheduleBlockRealize>()
                          ->schedule_block.As<ScheduleBlock>()
                          ->body;
    auto find_load_or_call =
        ir_utils::CollectIRNodesWithoutTensor(block_body, [&](const Expr* x) {
          if (x->As<Call>()) {
            const std::vector<Expr>& read_args = x->As<Call>()->read_args;
            for (const Expr& arg : read_args) {
              if (arg.as_tensor() &&
                  arg.as_tensor_ref()->name == store_tensor_name) {
                return true;
              }
            }
          }
          return x->As<Load>() && x->As<Load>()->tensor.as_tensor_ref()->name ==
                                      store_tensor_name;
        });
    if (!find_load_or_call.empty()) consumers.emplace_back(find_block);
  }
  return consumers;
}

std::vector<std::pair<Expr, Expr>> GetConsumerLoadsAndSBlocks(
    const Expr& block, const Expr& root) {
  PADDLE_ENFORCE_NOT_NULL(block.As<ScheduleBlockRealize>(),
                          ::common::errors::InvalidArgument(
                              "Failed to cast block to ScheduleBlockRealize."));
  PADDLE_ENFORCE_NOT_NULL(
      root.As<ScheduleBlockRealize>(),
      ::common::errors::InvalidArgument(
          "Failed to cast 'root' to ScheduleBlockRealize."));

  Expr store = GetStoreOfSBlock(block);
  std::vector<Expr> consumer_blocks = GetConsumerSBlocks(block, root);
  std::vector<std::pair<Expr, Expr>> loads_and_blocks;
  for (const Expr& consumer_block : consumer_blocks) {
    ir_utils::CollectIRNodesWithoutTensor(consumer_block, [&](const Expr* x) {
      if (x->As<Load>() &&
          (x->As<Load>()->name() == store.As<Store>()->name())) {
        loads_and_blocks.emplace_back(*x, consumer_block);
      }
      return false;
    });
  }
  return loads_and_blocks;
}

std::unordered_map<std::string, std::unordered_map<ir::Var, ir::Expr>>
CollectVarToForMap(const std::vector<Expr>& exprs,
                   const std::vector<Expr>& blocks) {
  std::unordered_map<std::string, std::unordered_map<ir::Var, ir::Expr>>
      for_map;
  for (const ir::Expr& block : blocks) {
    std::string block_name = block.As<ir::ScheduleBlockRealize>()
                                 ->schedule_block.As<ir::ScheduleBlock>()
                                 ->name;
    std::vector<ir::Expr> for_exprs = GetLoops(exprs, block);
    for (ir::Expr for_expr : for_exprs) {
      for_map[block_name][for_expr.As<ir::For>()->loop_var] = for_expr;
      VLOG(6) << "for_map.insert: <" << block_name << ", "
              << for_expr.As<ir::For>()->loop_var->name << ">";
    }
  }
  return for_map;
}

std::unordered_map<ir::Var, ir::Expr> GetIterVarToValueOfSBlock(
    ir::Expr block) {
  ir::ScheduleBlockRealize* s_block_realize =
      block.As<ir::ScheduleBlockRealize>();
  PADDLE_ENFORCE_NOT_NULL(s_block_realize,
                          ::common::errors::InvalidArgument(
                              "The block is not a ScheduleBlockRealize"));
  ir::ScheduleBlock* s_block =
      s_block_realize->schedule_block.As<ir::ScheduleBlock>();
  PADDLE_ENFORCE_NOT_NULL(
      s_block,
      ::common::errors::InvalidArgument("The block is not a ScheduleBlock"));
  PADDLE_ENFORCE_EQ(
      s_block_realize->iter_values.size(),
      s_block->iter_vars.size(),
      ::common::errors::InvalidArgument(
          "The size of iter_values should be equal to the size of "
          "iter_vars in the block!"));
  std::unordered_map<ir::Var, ir::Expr> iter_var2iter_values;
  for (size_t i = 0; i < s_block_realize->iter_values.size(); ++i) {
    iter_var2iter_values.emplace(s_block->iter_vars[i],
                                 s_block_realize->iter_values[i]);
  }
  return iter_var2iter_values;
}

ir::Expr ReplaceVarWithExpr(const ir::Expr& source,
                            const std::vector<ir::Var>& candidates,
                            const std::vector<ir::Expr>& targets) {
  PADDLE_ENFORCE_EQ(
      candidates.size(),
      targets.size(),
      ::common::errors::InvalidArgument(
          "In ReplaceExpr, the size of Vars to be replaces must "
          "be equal to the size of targets Exprs! Please check."));
  ir::Expr copied = ir::ir_utils::IRCopy(source);
  if (candidates.empty()) return copied;
  std::map<Var, Expr, CompVar> replacing_map;
  for (int i = 0; i < candidates.size(); ++i) {
    // If the Var to be candidates is equal to the candidate, we skip it.
    if (targets[i].is_var() && targets[i].as_var_ref() == candidates[i])
      continue;
    replacing_map[candidates[i]] = targets[i];
  }
  MappingVarToExprMutator mapper(replacing_map);
  mapper(&copied);
  return copied;
}

std::vector<ir::Expr> GetIterValuesOfAccess(ir::Expr load_or_store,
                                            ir::Expr block) {
  if (!load_or_store.As<ir::Load>())
    PADDLE_ENFORCE_NOT_NULL(
        load_or_store.As<ir::Store>(),
        ::common::errors::InvalidArgument(
            "Failed to cast 'load_or_store' to ir::Store type."));
  std::vector<ir::Expr> indices = load_or_store.As<ir::Load>()
                                      ? load_or_store.As<ir::Load>()->indices
                                      : load_or_store.As<ir::Store>()->indices;
  ir::ScheduleBlockRealize* s_block_realize =
      block.As<ir::ScheduleBlockRealize>();
  PADDLE_ENFORCE_NOT_NULL(s_block_realize,
                          ::common::errors::InvalidArgument(
                              "The block is not a ScheduleBlockRealize"));
  ir::ScheduleBlock* s_block =
      s_block_realize->schedule_block.As<ir::ScheduleBlock>();
  PADDLE_ENFORCE_NOT_NULL(
      s_block,
      ::common::errors::InvalidArgument("The block is not a ScheduleBlock"));

  std::vector<ir::Expr> iter_values;
  for (ir::Expr index : indices) {
    ir::Expr index_value = ReplaceVarWithExpr(
        index, s_block->iter_vars, s_block_realize->iter_values);
    iter_values.push_back(optim::ArithSimplify(index_value));
  }
  return iter_values;
}

std::unordered_set<ir::Var> GetReduceIterVars(ir::Expr block) {
  ir::ScheduleBlockRealize* schedule_block_realize =
      block.As<ir::ScheduleBlockRealize>();
  PADDLE_ENFORCE_NOT_NULL(schedule_block_realize,
                          ::common::errors::InvalidArgument(
                              "The block is not a ScheduleBlockRealize"));
  ir::ScheduleBlock* schedule_block =
      schedule_block_realize->schedule_block.As<ir::ScheduleBlock>();
  PADDLE_ENFORCE_NOT_NULL(
      schedule_block,
      ::common::errors::InvalidArgument("The block is not a ScheduleBlock"));
  std::vector<ir::Var>& iter_vars = schedule_block->iter_vars;
  std::unordered_set<ir::Var> reduce_vars;
  for (int i = 0; i < iter_vars.size(); ++i) {
    if (iter_vars[i]->is_reduce_axis) {
      reduce_vars.insert(iter_vars[i]);
    }
  }
  return reduce_vars;
}

bool IsReductionSBlock(ir::Expr block) {
  ir::ScheduleBlockRealize* s_block_realize =
      block.As<ir::ScheduleBlockRealize>();
  PADDLE_ENFORCE_NOT_NULL(s_block_realize,
                          ::common::errors::InvalidArgument(
                              "The block is not a ScheduleBlockRealize"));
  ir::ScheduleBlock* s_block =
      s_block_realize->schedule_block.As<ir::ScheduleBlock>();
  PADDLE_ENFORCE_NOT_NULL(
      s_block,
      ::common::errors::InvalidArgument("The block is not a ScheduleBlock"));
  for (const ir::Var& var : s_block->iter_vars) {
    if (var->is_reduce_axis) {
      return true;
    }
  }
  return false;
}

bool IsBroadcastSBlock(ir::Expr block) {
  ir::ScheduleBlockRealize* s_block_realize =
      block.As<ir::ScheduleBlockRealize>();
  PADDLE_ENFORCE_NOT_NULL(s_block_realize,
                          ::common::errors::InvalidArgument(
                              "The block is not a ScheduleBlockRealize"));
  ir::ScheduleBlock* s_block =
      s_block_realize->schedule_block.As<ir::ScheduleBlock>();
  PADDLE_ENFORCE_NOT_NULL(
      s_block,
      ::common::errors::InvalidArgument("The block is not a ScheduleBlock"));
  ir::Expr e_store = GetStoreOfSBlock(block);
  ir::Store* store = e_store.As<ir::Store>();
  PADDLE_ENFORCE_NOT_NULL(
      store,
      ::common::errors::InvalidArgument("The block is not a Store node"));
  ir::Load* load = store->value.As<ir::Load>();
  if (load == nullptr) {
    return false;
  }
  // each load index can be found in store index and maintain relative order
  const auto IsIndexZero = [](const ir::Expr& e) -> bool {
    return e.is_constant() && e.get_constant() == 0;
  };
  int num_load_index_zero = 0;
  for (size_t i = 0; i < load->indices.size(); ++i) {
    if (IsIndexZero(load->indices[i]) && i < store->indices.size() &&
        !IsIndexZero(store->indices[i])) {
      ++num_load_index_zero;
      continue;
    }
    bool found = false;
    for (size_t j = i; j < store->indices.size(); ++j) {
      ir::_Var_* load_var = load->indices[i].as_var();
      ir::_Var_* store_var = store->indices[j].as_var();
      if (load_var == nullptr || store_var == nullptr) {
        return false;
      }
      if (load_var->name == store_var->name) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return load->indices.size() - num_load_index_zero < store->indices.size();
}

std::vector<ir::Var> IndicesToVars(const std::vector<ir::Expr>& indices) {
  std::vector<ir::Var> result;
  for (const ir::Expr& e : indices) {
    if (e.is_constant()) {
      std::string var_name =
          cinn::UniqName("constant" + static_cast<int>(e.get_constant()));
      result.emplace_back(e, e, var_name, /* is_reduce = */ false);
    } else if (e.As<ir::_Var_>() != nullptr) {
      ir::Expr copy_e = ir::ir_utils::IRCopy(e);
      ir::_Var_* var_ref = copy_e.As<ir::_Var_>();
      result.emplace_back(ir::Var(var_ref));
    } else {
      std::string var_name = cinn::UniqName("expr");
      common::cas_intervals_t var_intervals;
      bool is_reduce = false;
      ir::ir_utils::CollectIRNodes(e, [&](const ir::Expr* x) {
        if (x->As<ir::_Var_>() != nullptr) {
          ir::Var var = x->as_var_ref();
          var_intervals.insert(
              {var->name,
               common::CasInterval{var->lower_bound, var->upper_bound}});
          if (var->is_reduce_axis) is_reduce = true;
        }
        return false;
      });
      common::SymbolicExprAnalyzer analyzer(var_intervals);
      result.emplace_back(
          analyzer.LowerBound(e), analyzer.UpperBound(e), var_name, is_reduce);
    }
  }
  return result;
}

void AnalyzeScheduleBlockReadWriteBuffer(ir::ScheduleBlock* sche_block) {
  if (!sche_block->read_buffers.empty() || !sche_block->write_buffers.empty()) {
    return;
  }

  ir::ir_utils::CollectIRNodesWithoutTensor(
      sche_block->body, [&](const Expr* x) {
        const ir::Load* load_expr = x->As<ir::Load>();
        if (load_expr != nullptr) {
          const ir::Tensor t = load_expr->tensor.as_tensor_ref();
          sche_block->read_buffers.emplace_back(
              ir::BufferRange(t->buffer, IndicesToVars(load_expr->indices)));
          return false;
        }
        const ir::Store* store_expr = x->As<ir::Store>();
        if (store_expr != nullptr) {
          const ir::Tensor t = store_expr->tensor.as_tensor_ref();
          sche_block->write_buffers.emplace_back(
              ir::BufferRange(t->buffer, IndicesToVars(store_expr->indices)));
          return false;
        }
        return false;
      });
}

std::string GetBlockName(const ir::Expr block) {
  const ir::ScheduleBlockRealize* block_realize =
      block.As<ir::ScheduleBlockRealize>();
  PADDLE_ENFORCE_NOT_NULL(block_realize,
                          ::common::errors::InvalidArgument(
                              "The block is not a ScheduleBlockRealize"));
  const ir::ScheduleBlock* block_node =
      block_realize->schedule_block.As<ir::ScheduleBlock>();
  PADDLE_ENFORCE_NOT_NULL(
      block_node,
      ::common::errors::InvalidArgument("The block is not a ScheduleBlock"));
  return block_node->name;
}

}  // namespace analyzer
}  // namespace ir
}  // namespace cinn
