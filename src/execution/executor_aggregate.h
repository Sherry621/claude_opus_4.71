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
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

// 聚合算子：消费孩子算子的全部元组，计算 COUNT/MAX/MIN/SUM 并输出一行结果
class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;   // 孩子算子（通常是带where的扫描）
    std::vector<TabCol> sel_cols_;             // 聚合选择列
    std::vector<ColMeta> output_cols_;         // 输出字段（name=别名，type=结果类型）
    size_t len_;                               // 输出记录长度
    bool is_end_;
    std::unique_ptr<RmRecord> result_;         // 计算得到的单行结果

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols) {
        prev_ = std::move(prev);
        sel_cols_ = std::move(sel_cols);

        const auto &child_cols = prev_->cols();
        size_t offset = 0;
        for (auto &sc : sel_cols_) {
            ColMeta out;
            out.tab_name = "";
            out.name = sc.alias.empty() ? sc.col_name : sc.alias;
            out.index = false;
            if (sc.agg_type == AGG_COUNT) {
                out.type = TYPE_INT;
                out.len = sizeof(int);
            } else {
                // SUM/MAX/MIN 的结果类型与源字段一致
                auto pos = get_col(child_cols, TabCol{sc.tab_name, sc.col_name});
                out.type = pos->type;
                out.len = pos->len;
            }
            out.offset = (int)offset;
            offset += out.len;
            output_cols_.push_back(out);
        }
        len_ = offset;
        is_end_ = false;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return output_cols_; }

    std::string getType() override { return "AggregateExecutor"; }

    void beginTuple() override {
        const auto &child_cols = prev_->cols();
        int n = (int)sel_cols_.size();
        std::vector<long long> cnt(n, 0);
        std::vector<double> sum(n, 0.0);
        std::vector<bool> has_val(n, false);
        std::vector<std::vector<char>> minmax(n);  // MAX/MIN 当前值的原始字节

        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            auto rec = prev_->Next();
            for (int i = 0; i < n; ++i) {
                auto &sc = sel_cols_[i];
                if (sc.agg_type == AGG_COUNT) {
                    cnt[i]++;  // COUNT(*) 与 COUNT(col)：本系统无NULL，逐行计数
                    continue;
                }
                auto pos = get_col(child_cols, TabCol{sc.tab_name, sc.col_name});
                char *data = rec->data + pos->offset;
                if (sc.agg_type == AGG_SUM) {
                    if (pos->type == TYPE_INT) sum[i] += *(int *)data;
                    else if (pos->type == TYPE_FLOAT) sum[i] += *(float *)data;
                    else if (pos->type == TYPE_BIGINT || pos->type == TYPE_DATETIME) sum[i] += (double)*(int64_t *)data;
                } else {  // AGG_MAX / AGG_MIN
                    if (!has_val[i]) {
                        minmax[i].assign(data, data + pos->len);
                        has_val[i] = true;
                    } else {
                        int cmp = compare_value(data, minmax[i].data(), pos->type, pos->len);
                        if ((sc.agg_type == AGG_MAX && cmp > 0) || (sc.agg_type == AGG_MIN && cmp < 0)) {
                            minmax[i].assign(data, data + pos->len);
                        }
                    }
                }
            }
        }

        // 写入结果记录
        result_ = std::make_unique<RmRecord>(len_);
        memset(result_->data, 0, len_);
        for (int i = 0; i < n; ++i) {
            auto &sc = sel_cols_[i];
            auto &out = output_cols_[i];
            char *dst = result_->data + out.offset;
            if (sc.agg_type == AGG_COUNT) {
                *(int *)dst = (int)cnt[i];
            } else if (sc.agg_type == AGG_SUM) {
                if (out.type == TYPE_INT) *(int *)dst = (int)sum[i];
                else if (out.type == TYPE_FLOAT) *(float *)dst = (float)sum[i];
                else if (out.type == TYPE_BIGINT || out.type == TYPE_DATETIME) *(int64_t *)dst = (int64_t)sum[i];
            } else {  // MAX / MIN
                if (has_val[i]) memcpy(dst, minmax[i].data(), out.len);
            }
        }
        is_end_ = false;
    }

    void nextTuple() override { is_end_ = true; }  // 聚合结果只有一行

    bool is_end() const override { return is_end_; }

    std::unique_ptr<RmRecord> Next() override {
        return std::make_unique<RmRecord>(*result_);
    }

    Rid &rid() override { return _abstract_rid; }
};
