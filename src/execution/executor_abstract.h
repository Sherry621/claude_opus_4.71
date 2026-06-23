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
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }

    /* 比较两个同类型的原始字节串，返回 <0 / 0 / >0 */
    static int compare_value(const char *lhs, const char *rhs, ColType type, int len) {
        switch (type) {
            case TYPE_INT: {
                int a = *(const int *)lhs;
                int b = *(const int *)rhs;
                return (a < b) ? -1 : (a > b ? 1 : 0);
            }
            case TYPE_BIGINT: {
                int64_t a = *(const int64_t *)lhs;
                int64_t b = *(const int64_t *)rhs;
                return (a < b) ? -1 : (a > b ? 1 : 0);
            }
            case TYPE_FLOAT: {
                float a = *(const float *)lhs;
                float b = *(const float *)rhs;
                return (a < b) ? -1 : (a > b ? 1 : 0);
            }
            case TYPE_STRING:
                return memcmp(lhs, rhs, len);
            default:
                return 0;
        }
    }

    /* 根据比较结果和比较运算符判断条件是否成立 */
    static bool eval_compare(int cmp, CompOp op) {
        switch (op) {
            case OP_EQ: return cmp == 0;
            case OP_NE: return cmp != 0;
            case OP_LT: return cmp < 0;
            case OP_GT: return cmp > 0;
            case OP_LE: return cmp <= 0;
            case OP_GE: return cmp >= 0;
            default: return false;
        }
    }

    /* 判断一条记录是否满足单个条件 */
    bool eval_cond(const std::vector<ColMeta> &rec_cols, const Condition &cond, const RmRecord *rec) {
        auto lhs_col = get_col(rec_cols, cond.lhs_col);
        const char *lhs_data = rec->data + lhs_col->offset;
        const char *rhs_data;
        if (cond.is_rhs_val) {
            rhs_data = cond.rhs_val.raw->data;
        } else {
            auto rhs_col = get_col(rec_cols, cond.rhs_col);
            rhs_data = rec->data + rhs_col->offset;
        }
        int cmp = compare_value(lhs_data, rhs_data, lhs_col->type, lhs_col->len);
        return eval_compare(cmp, cond.op);
    }

    /* 判断一条记录是否满足全部条件 */
    bool eval_conds(const std::vector<ColMeta> &rec_cols, const std::vector<Condition> &conds, const RmRecord *rec) {
        for (auto &cond : conds) {
            if (!eval_cond(rec_cols, cond, rec)) {
                return false;
            }
        }
        return true;
    }
};