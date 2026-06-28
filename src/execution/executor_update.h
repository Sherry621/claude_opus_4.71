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
#include <set>
#include <string>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    std::unique_ptr<RmRecord> Next() override {
        context_->lock_mgr_->lock_exclusive_on_table(context_->txn_, fh_->GetFd());
        // 预先为每个set子句的右值生成原始字节串
        for (auto &set : set_clauses_) {
            auto col = tab_.get_col(set.lhs.col_name);
            // 必要时进行隐式类型转换（int->bigint/float, bigint->int等）
            if (set.rhs.type != col->type) {
                if (!set.rhs.cast_to(col->type)) {
                    throw IncompatibleTypeError(coltype2str(col->type), coltype2str(set.rhs.type));
                }
            }
            if (set.rhs.raw == nullptr) {
                set.rhs.init_raw(col->len);
            }
        }

        // 唯一性约束预检查：先计算每条记录更新后的索引key，检查是否与其他记录冲突
        for (auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(
                sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            std::set<std::string> seen;
            for (auto &rid : rids_) {
                auto rec = fh_->get_record(rid, context_);
                // 应用set子句得到新记录
                for (auto &set : set_clauses_) {
                    auto col = tab_.get_col(set.lhs.col_name);
                    memcpy(rec->data + col->offset, set.rhs.raw->data, col->len);
                }
                std::string new_key(index.col_tot_len, '\0');
                int offset = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(&new_key[0] + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                // 与本批次中其他被更新记录冲突
                if (seen.count(new_key)) {
                    throw RMDBError("duplicate value violates unique index on " + tab_name_);
                }
                seen.insert(new_key);
                // 与索引中已有的、不属于本批次的记录冲突
                std::vector<Rid> found;
                if (ih->get_value(new_key.data(), &found, context_->txn_)) {
                    bool self = false;
                    for (auto &r : rids_) {
                        if (r == found[0]) { self = true; break; }
                    }
                    if (!self) {
                        throw RMDBError("duplicate value violates unique index on " + tab_name_);
                    }
                }
            }
        }

        for (auto &rid : rids_) {
            auto rec = fh_->get_record(rid, context_);

            // Record the update write operation for transaction rollback (saving the old record)
            context_->txn_->append_write_record(new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *rec));

            // 删除旧记录在各索引上的条目
            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(
                    sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                char *key = new char[index.col_tot_len];
                int offset = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(key + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                ih->delete_entry(key, context_->txn_);
                delete[] key;
            }

            // 在记录上应用set子句
            for (auto &set : set_clauses_) {
                auto col = tab_.get_col(set.lhs.col_name);
                memcpy(rec->data + col->offset, set.rhs.raw->data, col->len);
            }
            fh_->update_record(rid, rec->data, context_);

            // 插入更新后记录在各索引上的条目
            for (auto &index : tab_.indexes) {
                auto ih = sm_manager_->ihs_.at(
                    sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                char *key = new char[index.col_tot_len];
                int offset = 0;
                for (size_t i = 0; i < index.col_num; ++i) {
                    memcpy(key + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                    offset += index.cols[i].len;
                }
                ih->insert_entry(key, rid, context_->txn_);
                delete[] key;
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};