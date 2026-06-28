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
    size_t len_;                                // join后获得的每条记录 of length
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;

    std::vector<std::unique_ptr<RmRecord>> left_buffer_; // 左表（物化）的当前块的记录
    int left_pos_;                                      // 当前块中左表记录的下标
    std::unique_ptr<RmRecord> right_rec_;               // 当前匹配的右表记录

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

    void beginTuple() override {
        isend = false;
        left_->beginTuple();
        load_next_left_block();
        if (left_buffer_.empty()) {
            isend = true;
            return;
        }

        right_->beginTuple();
        if (right_->is_end()) {
            isend = true;
            return;
        }
        
        right_rec_ = right_->Next();
        left_pos_ = (int)left_buffer_.size() - 1;
        
        find_match();
    }

    void nextTuple() override {
        --left_pos_;
        find_match();
    }

    bool is_end() const override { return isend; }

    std::unique_ptr<RmRecord> Next() override {
        return join_record(left_buffer_[left_pos_].get(), right_rec_.get());
    }

    Rid &rid() override { return _abstract_rid; }

   private:
    void load_next_left_block() {
        left_buffer_.clear();
        size_t current_size = 0;
        size_t max_buffer_size = 50 * 1024 * 1024; // 50MB join buffer
        
        while (!left_->is_end()) {
            left_buffer_.push_back(left_->Next());
            current_size += left_->tupleLen();
            left_->nextTuple();
            if (current_size >= max_buffer_size) {
                break;
            }
        }
    }

    // 将当前左右记录拼接成一条连接后的记录
    std::unique_ptr<RmRecord> join_record(RmRecord* left_rec, RmRecord* right_rec) {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, left_rec->data, left_->tupleLen());
        memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
        return rec;
    }

    // 从当前左右位置开始，寻找下一对满足连接条件的记录
    void find_match() {
        while (true) {
            // Loop through the current left_buffer_ in reverse order for the current right_rec_
            while (right_rec_ != nullptr && left_pos_ >= 0) {
                auto rec = join_record(left_buffer_[left_pos_].get(), right_rec_.get());
                if (eval_conds(cols_, fed_conds_, rec.get())) {
                    // Match found!
                    return;
                }
                --left_pos_;
            }

            // If we reached the end of left_buffer_ (left_pos_ < 0), we need to advance right_
            if (!right_->is_end()) {
                right_->nextTuple();
            }

            // Check if right_ has reached the end
            if (right_->is_end()) {
                // Load the next block of left_
                load_next_left_block();
                if (left_buffer_.empty()) {
                    // No more blocks of left_, we are done!
                    isend = true;
                    return;
                }
                // Reset right_ to the beginning
                right_->beginTuple();
                if (right_->is_end()) {
                    // If right_ has no records at all, we are done!
                    isend = true;
                    return;
                }
                right_rec_ = right_->Next();
                left_pos_ = (int)left_buffer_.size() - 1;
            } else {
                // right_ is not end, get the next record of right_
                right_rec_ = right_->Next();
                left_pos_ = (int)left_buffer_.size() - 1;
            }
        }
    }
};