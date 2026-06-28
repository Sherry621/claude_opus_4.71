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

#include <climits>
#include <cfloat>
#include <cstdint>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "IndexScanExecutor"; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    void beginTuple() override {
        context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        auto ih = sm_manager_->ihs_.at(
            sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();

        int tot = index_meta_.col_tot_len;
        std::vector<char> low(tot), high(tot);
        bool low_inclusive = true, high_inclusive = true;
        bool prefix_open = true;
        int koff = 0;
        for (auto &icol : index_meta_.cols) {
            set_min(low.data() + koff, icol.type, icol.len);
            set_max(high.data() + koff, icol.type, icol.len);
            if (prefix_open) {
                bool has_eq = false, has_lo = false, has_hi = false, lo_inc = false, hi_inc = false;
                const char *eqv = nullptr, *lov = nullptr, *hiv = nullptr;
                for (auto &cond : conds_) {
                    if (!cond.is_rhs_val || cond.lhs_col.col_name != icol.name) continue;
                    const char *v = cond.rhs_val.raw->data;
                    switch (cond.op) {
                        case OP_EQ: has_eq = true; eqv = v; break;
                        case OP_GT: has_lo = true; lo_inc = false; lov = v; break;
                        case OP_GE: has_lo = true; lo_inc = true; lov = v; break;
                        case OP_LT: has_hi = true; hi_inc = false; hiv = v; break;
                        case OP_LE: has_hi = true; hi_inc = true; hiv = v; break;
                        default: break;
                    }
                }
                if (has_eq) {
                    memcpy(low.data() + koff, eqv, icol.len);
                    memcpy(high.data() + koff, eqv, icol.len);
                } else if (has_lo || has_hi) {
                    if (has_lo) { memcpy(low.data() + koff, lov, icol.len); low_inclusive = lo_inc; }
                    if (has_hi) { memcpy(high.data() + koff, hiv, icol.len); high_inclusive = hi_inc; }
                    prefix_open = false;  // 范围条件后停止扩展前缀
                } else {
                    prefix_open = false;  // 该列无条件，停止
                }
            }
            koff += icol.len;
        }

        Iid lower = low_inclusive ? ih->lower_bound(low.data()) : ih->upper_bound(low.data());
        Iid upper = high_inclusive ? ih->upper_bound(high.data()) : ih->lower_bound(high.data());
        scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
        find_next_valid();
    }

    void nextTuple() override {
        scan_->next();
        find_next_valid();
    }

    bool is_end() const override { return scan_->is_end(); }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    Rid &rid() override { return rid_; }

   private:
    // 从当前scan位置向后寻找第一条满足全部条件的记录
    void find_next_valid() {
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conds(cols_, fed_conds_, rec.get())) {
                return;
            }
            scan_->next();
        }
    }

    static void set_min(char *p, ColType t, int len) {
        switch (t) {
            case TYPE_INT: *(int *)p = INT_MIN; break;
            case TYPE_BIGINT:
            case TYPE_DATETIME: *(int64_t *)p = INT64_MIN; break;
            case TYPE_FLOAT: *(float *)p = -FLT_MAX; break;
            case TYPE_STRING: memset(p, 0, len); break;
            default: break;
        }
    }

    static void set_max(char *p, ColType t, int len) {
        switch (t) {
            case TYPE_INT: *(int *)p = INT_MAX; break;
            case TYPE_BIGINT:
            case TYPE_DATETIME: *(int64_t *)p = INT64_MAX; break;
            case TYPE_FLOAT: *(float *)p = FLT_MAX; break;
            case TYPE_STRING: memset(p, (char)0xff, len); break;
            default: break;
        }
    }
};