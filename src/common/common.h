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

#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;
    AggType agg_type = AGG_NONE;   // 聚合类型
    std::string alias;             // as别名（用于输出表头）
    bool is_star = false;          // 是否为 COUNT(*)

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

/**
 * @description: 解析并校验 'YYYY-MM-DD HH:MM:SS' 格式的时间字符串
 * @return {bool} 是否为合法时间；合法时通过out返回编码后的int64值(YYYYMMDDHHMMSS)
 * 取值范围 '1000-01-01 00:00:00' ~ '9999-12-31 23:59:59'
 */
inline bool parse_datetime(const std::string &s, int64_t &out) {
    if (s.size() != 19) return false;  // 字段长度必须严格为19
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    // 分隔符位置校验
    if (s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':') return false;
    // 数字位置校验（同时排除负号等非法字符）
    const int digit_pos[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    for (int p : digit_pos) {
        if (!is_digit(s[p])) return false;
    }
    int year = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int month = (s[5]-'0')*10 + (s[6]-'0');
    int day = (s[8]-'0')*10 + (s[9]-'0');
    int hour = (s[11]-'0')*10 + (s[12]-'0');
    int minute = (s[14]-'0')*10 + (s[15]-'0');
    int second = (s[17]-'0')*10 + (s[18]-'0');
    if (year < 1000 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (hour > 23) return false;
    if (minute > 59) return false;
    if (second > 59) return false;
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int dim = month_days[month - 1];
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) dim = 29;
    if (day < 1 || day > dim) return false;
    out = ((((int64_t)year * 100 + month) * 100 + day) * 100 + hour) * 100 + minute;
    out = out * 100 + second;
    return true;
}

/* 将编码后的datetime(int64)还原为 'YYYY-MM-DD HH:MM:SS' 字符串 */
inline std::string datetime_to_str(int64_t v) {
    int second = (int)(v % 100); v /= 100;
    int minute = (int)(v % 100); v /= 100;
    int hour = (int)(v % 100); v /= 100;
    int day = (int)(v % 100); v /= 100;
    int month = (int)(v % 100); v /= 100;
    int year = (int)v;
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return std::string(buf);
}

struct Value {
    ColType type;  // type of value
    union {
        int int_val;            // int value
        float float_val;        // float value
        int64_t bigint_val;     // bigint value (8字节有符号整数)
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_bigint(int64_t bigint_val_) {
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }

    void set_datetime(int64_t datetime_val_) {
        type = TYPE_DATETIME;
        bigint_val = datetime_val_;  // datetime编码后的int64复用bigint_val槽位
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    /**
     * @description: 将当前值转换为目标列类型，必要时进行隐式类型转换
     * @return {bool} 转换是否成功（超出目标类型范围时返回false）
     */
    bool cast_to(ColType target) {
        if (type == target) {
            return true;
        }
        if (target == TYPE_BIGINT && type == TYPE_INT) {
            set_bigint((int64_t)int_val);
            return true;
        }
        if (target == TYPE_INT && type == TYPE_BIGINT) {
            if (bigint_val < INT_MIN || bigint_val > INT_MAX) {
                return false;  // 超出INT范围
            }
            set_int((int)bigint_val);
            return true;
        }
        if (target == TYPE_FLOAT && type == TYPE_INT) {
            set_float((float)int_val);
            return true;
        }
        if (target == TYPE_FLOAT && type == TYPE_BIGINT) {
            set_float((float)bigint_val);
            return true;
        }
        if (target == TYPE_DATETIME && type == TYPE_STRING) {
            int64_t dt;
            if (!parse_datetime(str_val, dt)) {
                return false;  // 非法时间字符串
            }
            set_datetime(dt);
            return true;
        }
        return false;
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_BIGINT || type == TYPE_DATETIME) {
            assert(len == sizeof(int64_t));
            *(int64_t *)(raw->data) = bigint_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};