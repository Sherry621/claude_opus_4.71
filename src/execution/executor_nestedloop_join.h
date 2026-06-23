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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

    std::vector<std::unique_ptr<RmRecord>> left_buf_;   // 左表物化后的全部记录
    int left_pos_;                                      // 当前内层（左表）记录的下标

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);

    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "NestedLoopJoinExecutor"; }

    // 右表为外层循环，左表（物化后）为内层循环
    void beginTuple() override {
        // 将左表的全部记录物化到缓冲区
        left_buf_.clear();
        for (left_->beginTuple(); !left_->is_end(); left_->nextTuple()) {
            left_buf_.push_back(left_->Next());
        }
        right_->beginTuple();
        left_pos_ = (int)left_buf_.size() - 1;
        isend = false;
        find_match();
    }

    void nextTuple() override {
        --left_pos_;
        find_match();
    }

    bool is_end() const override { return isend; }

    std::unique_ptr<RmRecord> Next() override {
        return join_record();
    }

    Rid &rid() override { return _abstract_rid; }

   private:
    // 将当前左右记录拼接成一条连接后的记录
    std::unique_ptr<RmRecord> join_record() {
        auto right_rec = right_->Next();
        auto &left_rec = left_buf_[left_pos_];
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    // 从当前左右位置开始，寻找下一对满足连接条件的记录
    void find_match() {
        while (!right_->is_end()) {
            while (left_pos_ >= 0) {
                auto rec = join_record();
                if (eval_conds(cols_, fed_conds_, rec.get())) {
                    return;
                }
                --left_pos_;
            }
            // 内层表扫描完毕，外层表前进一行，内层表重置
            right_->nextTuple();
            left_pos_ = (int)left_buf_.size() - 1;
        }
        isend = true;
    }
};