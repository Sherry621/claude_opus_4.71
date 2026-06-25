/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include <algorithm>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// 排序算子：物化孩子算子的全部记录，按多列排序键(可分别升/降序)排序后逐条输出
class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> order_cols_;                  // 排序键在孩子输出中的元数据
    std::vector<bool> is_desc_;                        // 每个排序键是否降序
    std::vector<std::unique_ptr<RmRecord>> tuples_;    // 物化的全部记录
    size_t pos_;                                       // 当前输出位置

   public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols, std::vector<bool> is_desc) {
        prev_ = std::move(prev);
        for (auto &c : sel_cols) {
            order_cols_.push_back(*get_col(prev_->cols(), c));
        }
        is_desc_ = std::move(is_desc);
        pos_ = 0;
    }

    size_t tupleLen() const override { return prev_->tupleLen(); }

    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }

    std::string getType() override { return "SortExecutor"; }

    void beginTuple() override {
        tuples_.clear();
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            tuples_.push_back(prev_->Next());
        }
        std::stable_sort(tuples_.begin(), tuples_.end(),
                         [this](const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b) {
                             return compare_tuple(a.get(), b.get()) < 0;
                         });
        pos_ = 0;
    }

    void nextTuple() override { pos_++; }

    bool is_end() const override { return pos_ >= tuples_.size(); }

    std::unique_ptr<RmRecord> Next() override {
        return std::make_unique<RmRecord>(*tuples_[pos_]);
    }

    Rid &rid() override { return _abstract_rid; }

   private:
    // 按多列排序键比较两条记录
    int compare_tuple(const RmRecord *a, const RmRecord *b) {
        for (size_t i = 0; i < order_cols_.size(); ++i) {
            auto &col = order_cols_[i];
            int c = compare_value(a->data + col.offset, b->data + col.offset, col.type, col.len);
            if (c != 0) {
                return is_desc_[i] ? -c : c;
            }
        }
        return 0;
    }
};