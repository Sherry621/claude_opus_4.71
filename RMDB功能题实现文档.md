# RMDB 功能题实现文档

> 本文档记录在 RMDB（人大金仓系统能力大赛框架）基础上完成的 6 道功能题，均为「前置题目：题目二（查询执行）」的后续扩展。每道题贯穿 **词法 → 语法 → AST → 语义分析 → 查询计划 → 执行算子 → 结果输出** 的完整链路。
>
> 全部题目均在 WSL2 Ubuntu 下用 g++/flex/bison 编译，并以逐字节比对期望输出的方式验证通过。

---

## 目录

- [0. 总览与通用环境](#0-总览与通用环境)
- [1. 题目二：查询执行（元数据 + DDL + DML + DQL）](#1-题目二查询执行)
- [2. BIGINT 大整数类型](#2-bigint-大整数类型)
- [3. DATETIME 时间类型](#3-datetime-时间类型)
- [4. B+ 树索引](#4-b-树索引)
- [5. 聚合函数 COUNT/MAX/MIN/SUM](#5-聚合函数)
- [6. ORDER BY + LIMIT](#6-order-by--limit)
- [7. 基础设施 Bug 修复](#7-基础设施-bug-修复)
- [8. 验收 Q&A](#8-验收-qa)

---

## 0. 总览与通用环境

### 0.1 框架结构

```
src/
  parser/      词法(lex.l)、语法(yacc.y)、AST(ast.h)
  analyze/     语义分析，把 AST 转成 Query
  optimizer/   查询计划生成(planner.cpp)、优化器(optimizer.h)、计划结构(plan.h)
  execution/   各类执行算子(executor_*.h)、执行管理(execution_manager.cpp)
  system/      元数据管理与DDL(sm_manager.cpp/.h、sm_meta.h)
  index/       B+树索引(ix_index_handle.cpp、ix_scan.cpp、ix_manager.h)
  record/      记录存储(rm_file_handle.cpp、rm_scan.cpp)
  storage/     缓冲池(buffer_pool_manager.cpp)、磁盘管理
  transaction/ 事务与并发
  common/      公共结构(common.h: Value/TabCol/Condition)
  defs.h       基础类型定义(ColType 等)
  portal.h     把查询计划转成算子树并执行
  rmdb.cpp     服务端主程序
```

### 0.2 一条 SQL 的执行流（关键，贯穿所有题目）

```
客户端SQL字符串
  → yacc/lex 解析 → ast::TreeNode（语法树）
  → Analyze::do_analyze → Query（语义检查后的中间结构）
  → Optimizer::plan_query → Planner::do_planner → Plan（查询计划树）
  → Portal::start → AbstractExecutor 算子树
  → Portal::run → QlManager::select_from / run_dml / run_mutli_query / run_cmd_utility
  → 结果写入 output.txt 并返回客户端
```

> **理解这条链路是所有题目的基础**：新增一个类型/函数/子句，就是在这条链路的每一层补一块。

### 0.3 编译

在 **WSL2 Ubuntu** 中操作（项目位于 Windows 盘，通过 /mnt/c 访问）：

```bash
# 进入 WSL
wsl
cd "/mnt/c/Users/Sherry Peng/OneDrive/桌面/claude_opus_4.7"

# 首次需要装依赖
sudo apt-get update
sudo apt-get install -y gcc g++ cmake make flex bison

# 构建
cd build
cmake ..        # 首次或改了 CMakeLists 时
make rmdb -j4   # 编译服务端；改了 lex.l/yacc.y 会自动重新生成解析器
```

可执行文件在 `build/bin/rmdb`，客户端在 `rmdb_client/build/rmdb_client`。

### 0.4 通用测试方法

服务端 + 客户端两个进程。**关键三步**：

```bash
# 1. 杀掉残留服务端（否则端口 8765 被占，新服务端 Bind error，客户端会连到旧进程）
pkill -9 rmdb

# 2. 用全新库名启动服务端（服务端会 chdir 进入该库目录）
cd "/mnt/c/Users/Sherry Peng/OneDrive/桌面/claude_opus_4.7/build"
rm -rf <db_name>
./bin/rmdb <db_name>

# 3. 另一个终端连客户端，逐条输入 SQL，最后 exit
cd "../rmdb_client/build"
./rmdb_client
```

输出写入 **`build/<db_name>/output.txt`**（追加模式）。

**自动化脚本**（`build/run_test.sh`，已包含「删旧库 → 起服务 → 喂SQL → 关服务 → 打印 output.txt」）：

```bash
bash run_test.sh <db_name> <sql_file>
```

> ⚠️ 注意：
> 1. 只有 `select` / `show tables` / `show index` 以及**非法语句(failure)** 才会写 output.txt；成功的 create/insert/update/delete 不产生输出。
> 2. output.txt 是追加写，复测前要删库。
> 3. 浮点输出 6 位小数（`std::to_string(float)`），整数无小数。

### 0.5 输出格式（评测比对的就是它）

由框架的 `select_from`（[execution_manager.cpp](src/execution/execution_manager.cpp)）和 `show_tables`/`show_index`（[sm_manager.cpp](src/system/sm_manager.cpp)）显式写入：

- 表头：`| 列名1 | 列名2 |`
- 数据行：`| 值1 | 值2 |`（每个值左右各一个空格）
- 非法 SQL：`failure`

---

## 1. 题目二：查询执行

### 1.1 题目分析

在题目一（存储管理）基础上增加查询执行模块，使系统支持：
- **元数据管理**：open_db / close_db / drop_table
- **DDL**：create table / drop table
- **DML**：insert / delete / update
- **DQL**：select（单表条件查询、多表连接查询）
- 所有非法 SQL 输出 `failure`（语法错误、删不存在的表、建已存在的表、where 中不存在的字段等）
- 注意浮点精度

5 个测试点：建表、单表插入与条件查询、单表更新与条件查询、单表删除与条件查询、连接查询。

### 1.2 涉及改动

| 文件 | 改动内容 |
|------|---------|
| [src/system/sm_manager.cpp](src/system/sm_manager.cpp) | 实现 `open_db`、`close_db`、`drop_table` |
| [src/analyze/analyze.cpp](src/analyze/analyze.cpp) | select/delete 表存在性检查、`check_column` 指定表名时校验、补全 update 语义、where 类型隐式转换 |
| [src/execution/executor_abstract.h](src/execution/executor_abstract.h) | 新增条件求值公共函数 `compare_value`/`eval_compare`/`eval_cond`/`eval_conds` |
| [src/execution/executor_seq_scan.h](src/execution/executor_seq_scan.h) | 顺序扫描算子 |
| [src/execution/executor_projection.h](src/execution/executor_projection.h) | 投影算子 |
| [src/execution/executor_nestedloop_join.h](src/execution/executor_nestedloop_join.h) | 嵌套循环连接算子 |
| [src/execution/executor_update.h](src/execution/executor_update.h)、[executor_delete.h](src/execution/executor_delete.h) | 更新/删除算子（含索引维护） |
| [src/transaction/transaction_manager.cpp](src/transaction/transaction_manager.cpp) | 补全 begin/commit/abort（基础设施，见第 7 节） |

### 1.3 关键代码解析

#### (1) 元数据管理 — `sm_manager.cpp`

`open_db`：进入库目录、从 `db.meta` 反序列化元数据、打开每张表的数据文件与索引文件。

```cpp
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) throw DatabaseNotFoundError(db_name);
    if (chdir(db_name.c_str()) < 0) throw UnixError();   // 进入库目录，之后 output.txt 就写在这
    std::ifstream ifs(DB_META_NAME);
    ifs >> db_;                                          // 反序列化（sm_meta.h 重载了 >>）
    ifs.close();
    for (auto &entry : db_.tabs_) {                      // 打开每张表的数据文件和索引
        auto &tab = entry.second;
        fhs_.emplace(tab.name, rm_manager_->open_file(tab.name));
        for (auto &index : tab.indexes) {
            auto index_name = ix_manager_->get_index_name(tab.name, index.cols);
            ihs_.emplace(index_name, ix_manager_->open_index(tab.name, index.cols));
        }
    }
}
```

`close_db`：刷元数据、关闭所有文件、清内存、回上级目录。`drop_table`：校验存在→关闭并销毁索引/数据文件→从元数据删除→落盘。

#### (2) 条件求值公共方法 — `executor_abstract.h`

所有扫描/连接算子共用的谓词求值逻辑（按类型比较原始字节）：

```cpp
static int compare_value(const char *lhs, const char *rhs, ColType type, int len) {
    switch (type) {
        case TYPE_INT:    { int a=*(int*)lhs,   b=*(int*)rhs;   return a<b?-1:(a>b?1:0); }
        case TYPE_FLOAT:  { float a=*(float*)lhs,b=*(float*)rhs; return a<b?-1:(a>b?1:0); }
        case TYPE_STRING: return memcmp(lhs, rhs, len);
        // bigint/datetime 见后续题目
    }
}
bool eval_conds(const std::vector<ColMeta>& cols, const std::vector<Condition>& conds, const RmRecord* rec) {
    for (auto& cond : conds) if (!eval_cond(cols, cond, rec)) return false;
    return true;   // 所有条件 AND
}
```

#### (3) 顺序扫描算子 — `executor_seq_scan.h`

火山模型：`beginTuple`/`nextTuple`/`is_end`/`Next`，跳过不满足条件的记录。

```cpp
void beginTuple() override { scan_ = std::make_unique<RmScan>(fh_); find_next_valid(); }
void nextTuple()  override { scan_->next(); find_next_valid(); }
bool is_end() const override { return scan_->is_end(); }
std::unique_ptr<RmRecord> Next() override { return fh_->get_record(rid_, context_); }
void find_next_valid() {            // 向后找第一条满足 fed_conds_ 的记录
    while (!scan_->is_end()) {
        rid_ = scan_->rid();
        auto rec = fh_->get_record(rid_, context_);
        if (eval_conds(cols_, fed_conds_, rec.get())) return;
        scan_->next();
    }
}
```

#### (4) 嵌套循环连接算子 — `executor_nestedloop_join.h`

**关键点**：planner 对无谓词笛卡尔积生成 `left=右表, right=左表` 的计划。为让 `select * from t,d` 的输出行序与期望一致（外层=t、内层=d 且逆序），采用「**右表为外层循环、左表物化后逆序为内层**」：

```cpp
void beginTuple() override {
    left_buf_.clear();
    for (left_->beginTuple(); !left_->is_end(); left_->nextTuple())
        left_buf_.push_back(left_->Next());     // 物化左表
    right_->beginTuple();
    left_pos_ = (int)left_buf_.size() - 1;      // 内层逆序
    isend = false;
    find_match();
}
std::unique_ptr<RmRecord> join_record() {       // 拼接 left||right
    auto right_rec = right_->Next();
    auto &left_rec = left_buf_[left_pos_];
    auto rec = std::make_unique<RmRecord>(len_);
    memcpy(rec->data, left_rec->data, left_->tupleLen());
    memcpy(rec->data + left_->tupleLen(), right_rec->data, right_->tupleLen());
    return rec;
}
```

该实现同时满足两条 join 查询的期望输出（笛卡尔积 + 带谓词连接）。

#### (5) update 语义补全 — `analyze.cpp`

把 `set col = val` 中的值按列类型校验/隐式转换；where 条件同理。

```cpp
} else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
    if (!sm_manager_->db_.is_table(x->tab_name)) throw TableNotFoundError(x->tab_name);
    TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
    for (auto &sv_set : x->set_clauses) {
        SetClause set_clause;
        set_clause.lhs = {.tab_name = x->tab_name, .col_name = sv_set->col_name};
        auto col = tab.get_col(sv_set->col_name);          // 列不存在 → 抛异常 → failure
        set_clause.rhs = convert_sv_value(sv_set->val);
        if (set_clause.rhs.type != col->type)              // 隐式类型转换（如 set score=0，0是int但列是float）
            if (!set_clause.rhs.cast_to(col->type)) throw IncompatibleTypeError(...);
        query->set_clauses.push_back(set_clause);
    }
    get_clause(x->conds, query->conds);
    check_clause({x->tab_name}, query->conds);
}
```

### 1.4 测试方法与指令

```bash
cd "/mnt/c/Users/Sherry Peng/OneDrive/桌面/claude_opus_4.7/build"
pkill -9 rmdb

cat > t.sql <<'EOF'
create table grade (name char(4),id int,score float);
insert into grade values ('Data', 1, 90.5);
insert into grade values ('Data', 2, 95.0);
insert into grade values ('Calc', 2, 92.0);
insert into grade values ('Calc', 1, 88.5);
select * from grade;
select score,name,id from grade where score > 90;
update grade set score = 99.0 where name = 'Calc';
select * from grade;
EOF
bash run_test.sh execution_test_db t.sql
```

连接查询（测试点5）：
```sql
create table t ( id int , t_name char (3));
create table d (d_name char(5),id int);
insert into t values (1,'aaa'); insert into t values (2,'baa'); insert into t values (3,'bba');
insert into d values ('12345',1); insert into d values ('23456',2);
select * from t, d;
select t.id,t_name,d_name from t,d where t.id = d.id;
```

### 1.5 易错点

- `score > 90`：90 是 int，score 是 float → 必须做 int→float 隐式转换，否则报类型不兼容。
- `set score = 0`：0 是 int 字面量赋给 float 列 → 转换为 0.0，输出 `0.000000`。
- 浮点输出 6 位小数；整数无小数。

---

## 2. BIGINT 大整数类型

### 2.1 题目分析

实现 8 字节有符号整数 BIGINT（范围同 MySQL：-9223372036854775808 ~ 9223372036854775807），支持增删改查；超出 int64 范围的字面量（如 9223372036854775809）输出 `failure`。

### 2.2 设计核心

BIGINT 内部用 **int64_t** 存储。整型字面量在词法层按大小区分：落在 int32 范围内仍是 `VALUE_INT`，否则是 `VALUE_BIGINT`（含溢出标记），溢出交由语义层判 failure。

### 2.3 涉及改动（全栈）

| 层 | 文件 | 改动 |
|----|------|------|
| 类型 | [src/defs.h](src/defs.h) | `ColType` 末尾加 `TYPE_BIGINT`（放末尾保证已有元数据序列化值不变）；`coltype2str` 加 `BIGINT` |
| 值 | [src/common/common.h](src/common/common.h) | `Value` 联合体加 `int64_t bigint_val`；`set_bigint`、`init_raw`、`cast_to` |
| 词法 | [src/parser/lex.l](src/parser/lex.l) | `BIGINT` 关键字；整型字面量 strtoll 后按 int32 范围分流 |
| 语法/AST | [src/parser/yacc.y](src/parser/yacc.y)、[ast.h](src/parser/ast.h) | `BIGINT` 类型、`VALUE_BIGINT` token、`BigintLit` 节点（构造时检测越界） |
| 语义 | [src/analyze/analyze.cpp](src/analyze/analyze.cpp) | 越界字面量抛异常；where/set 隐式转换 |
| 算子/输出 | executor_abstract/insert/update、[execution_manager.cpp](src/execution/execution_manager.cpp)、[ix_index_handle.h](src/index/ix_index_handle.h) | 比较、插入/更新转换、输出、索引比较 |
| 映射 | [planner.h](src/optimizer/planner.h)、[ast_printer.h](src/parser/ast_printer.h) | `SV_TYPE_BIGINT → TYPE_BIGINT` |

### 2.4 关键代码解析

#### (1) 词法层按大小分流 — `lex.l`

```cpp
{value_int} {
    errno = 0;
    long long v = strtoll(yytext, nullptr, 10);
    if (errno != ERANGE && v >= INT_MIN && v <= INT_MAX) {
        yylval->sv_int = (int)v; return VALUE_INT;       // 普通 int
    } else {
        yylval->sv_str = yytext; return VALUE_BIGINT;    // 大整数（文本传给语法层）
    }
}
```

#### (2) BigintLit 节点检测越界 — `ast.h`

```cpp
struct BigintLit : public Value {
    int64_t val;
    bool overflow;   // 是否超出 int64
    BigintLit(const std::string &s) {
        errno = 0;
        val = std::strtoll(s.c_str(), nullptr, 10);
        overflow = (errno == ERANGE);   // 9223372036854775809 → ERANGE
    }
};
```

#### (3) 语义层判 failure — `analyze.cpp`

```cpp
} else if (auto bigint_lit = std::dynamic_pointer_cast<ast::BigintLit>(sv_val)) {
    if (bigint_lit->overflow) throw RMDBError("bigint value out of range");  // → failure
    val.set_bigint(bigint_lit->val);
}
```

> 注意：越界必须在 **语义/执行阶段抛 RMDBError**（被 rmdb.cpp 的 catch 写 failure），而不是让词法/语法解析失败 —— 因为语法失败不会写 failure。

#### (4) 隐式转换 `Value::cast_to` — `common.h`

```cpp
bool cast_to(ColType target) {
    if (type == target) return true;
    if (target == TYPE_BIGINT && type == TYPE_INT) { set_bigint((int64_t)int_val); return true; }
    if (target == TYPE_INT && type == TYPE_BIGINT) {
        if (bigint_val < INT_MIN || bigint_val > INT_MAX) return false;  // 超 int 范围 → failure
        set_int((int)bigint_val); return true;
    }
    // ... int→float, bigint→float
    return false;
}
```

### 2.5 测试

```sql
CREATE TABLE t(bid bigint,sid int);
INSERT INTO t VALUES(372036854775807,233421);
INSERT INTO t VALUES(-922337203685477580,124332);
SELECT * FROM t;
INSERT INTO t VALUES(9223372036854775809,12345);   -- 越界 → failure
SELECT * FROM t;
```

---

## 3. DATETIME 时间类型

### 3.1 题目分析

实现 8 字节 DATETIME，格式 `'YYYY-MM-DD HH:MM:SS'`，范围 `1000-01-01 00:00:00` ~ `9999-12-31 23:59:59`，支持增删改查，并校验合法性（负值、月<1或>12、日越界、2月天数、时>23、分秒>59、长度不匹配等）。

### 3.2 设计核心

DATETIME 内部用 **int64 编码** `YYYYMMDDHHMMSS`（如 `2023-05-18 09:12:19` → `20230518091219`）：
- 这种编码**保持时间先后顺序**，比较/相等用 int64 比较即可（复用 BIGINT 的比较分支）；
- 输出时格式化回字符串。
- 字面量本身是字符串，**无需新 token**，在绑定到 DATETIME 列时把字符串解析+校验成 int64。

### 3.3 涉及改动

| 层 | 文件 | 改动 |
|----|------|------|
| 类型 | [src/defs.h](src/defs.h) | `TYPE_DATETIME` + `coltype2str` |
| 值/工具 | [src/common/common.h](src/common/common.h) | `parse_datetime`（解析+校验）、`datetime_to_str`（还原）；`Value` 加 `set_datetime`、`cast_to(STRING→DATETIME)`、`init_raw` |
| 词法/语法/AST | [lex.l](src/parser/lex.l)、[yacc.y](src/parser/yacc.y)、[ast.h](src/parser/ast.h) | `DATETIME` 关键字 + 类型规则（len 8） |
| 算子/输出 | [executor_abstract.h](src/execution/executor_abstract.h)、[execution_manager.cpp](src/execution/execution_manager.cpp)、[ix_index_handle.h](src/index/ix_index_handle.h) | 比较、输出格式化、索引比较 |
| 映射 | [planner.h](src/optimizer/planner.h)、[ast_printer.h](src/parser/ast_printer.h) | 类型映射 |

> analyze 无需改动 —— 复用 BIGINT 时做的通用 `cast_to` 强制转换路径（insert/where/set 三处）。

### 3.4 关键代码解析

#### (1) 解析与校验 — `common.h`

```cpp
inline bool parse_datetime(const std::string &s, int64_t &out) {
    if (s.size() != 19) return false;                          // 长度严格 19
    if (s[4]!='-'||s[7]!='-'||s[10]!=' '||s[13]!=':'||s[16]!=':') return false;  // 分隔符
    const int digit_pos[] = {0,1,2,3,5,6,8,9,11,12,14,15,17,18};
    for (int p : digit_pos) if (!(s[p]>='0'&&s[p]<='9')) return false;  // 排除负号等
    int year=..., month=..., day=..., hour=..., minute=..., second=...;
    if (year < 1000 || year > 9999) return false;
    if (month < 1 || month > 12) return false;
    if (hour > 23 || minute > 59 || second > 59) return false;
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int dim = month_days[month-1];
    bool leap = (year%4==0 && year%100!=0) || (year%400==0);
    if (month == 2 && leap) dim = 29;                          // 闰年 2 月 29 天
    if (day < 1 || day > dim) return false;                    // 1999-02-30 非法
    out = ((((int64_t)year*100+month)*100+day)*100+hour)*100+minute;
    out = out*100 + second;
    return true;
}
```

非法时 `cast_to` 返回 false → 抛 RMDBError → 写 failure。因校验在写入前完成，失败插入不影响表数据。

#### (2) 输出格式化 — `common.h`

```cpp
inline std::string datetime_to_str(int64_t v) {
    int second=v%100; v/=100; int minute=v%100; v/=100; int hour=v%100; v/=100;
    int day=v%100; v/=100; int month=v%100; v/=100; int year=(int)v;
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
    return std::string(buf);   // 补前导零
}
```

### 3.5 测试

```sql
create table t(id int , time datetime);
insert into t values(1, '2023-05-18 09:12:19');
select * from t;
delete from t where time = '2023-05-30 12:34:32';   -- datetime 相等比较
-- 非法输入全部 failure：
insert into t values('1999-13-07 12:30:00', 36.0);  -- 月13
insert into t values('1999-1-07 12:30:00', 36.0);   -- 长度18
insert into t values('1999-02-30 12:30:00', 36.0);  -- 2月30
insert into t values('1999-02-28 12:30:61', 36.0);  -- 秒61
```

---

## 4. B+ 树索引

### 4.1 题目分析

在系统中增加**唯一索引**，支持：创建/删除/展示索引、单点查询、范围查询、索引与基表同步。要求：
1. 单/多字段索引的创建删除；`show index from t` 输出 `| t | unique | (col,col) |`（逗号后无空格）。
2. 索引查询：**最左匹配原则**（联合索引前缀匹配，能自动调整 where 顺序），且真正走索引（性能 < 全表扫描 70%）。
3. 索引维护：增删改时同步更新索引，并检查唯一性约束。

### 4.2 工作量说明

框架在 `src/index/ix_index_handle.cpp` 中提供了 B+ 树的**接口声明，但实现全是空桩**（`return -1/false/nullptr`）。本题需要**从零实现完整 B+ 树**（插入分裂、删除合并/重分配），外加索引扫描算子、最左匹配、DDL、唯一性约束。

### 4.3 涉及改动

| 模块 | 文件 | 改动 |
|------|------|------|
| B+树本体 | [src/index/ix_index_handle.cpp](src/index/ix_index_handle.cpp) | 实现全部空桩函数（见下） |
| 索引扫描 | [src/execution/executor_index_scan.h](src/execution/executor_index_scan.h) | 最左匹配构造扫描区间 + 残差过滤 |
| 索引 DDL | [src/system/sm_manager.cpp](src/system/sm_manager.cpp) | `create_index`/`drop_index`(两重载)/`show_index` |
| show index | [lex.l](src/parser/lex.l)/[yacc.y](src/parser/yacc.y)/[ast.h](src/parser/ast.h)/[optimizer.h](src/optimizer/optimizer.h)/[plan.h](src/optimizer/plan.h)/[execution_manager.cpp](src/execution/execution_manager.cpp) | 新增 `SHOW INDEX FROM t` 全链路 |
| 最左匹配 | [src/optimizer/planner.cpp](src/optimizer/planner.cpp) | 重写 `get_index_cols` |
| 唯一性 | [executor_insert.h](src/execution/executor_insert.h)/[executor_update.h](src/execution/executor_update.h) | 插入/更新前唯一性预检查 |
| Bug 修复 | [buffer_pool_manager.cpp](src/storage/buffer_pool_manager.cpp) 等 | `delete_all_pages`（见第 7 节） |

### 4.4 B+ 树关键实现

#### 节点布局

每个页面 = `IxPageHdr`(头) + `keys`(键数组) + `rids`(值数组)。内部节点的 `rid.page_no` 存孩子页号；叶子节点的 `rid` 存基表记录位置。本设计中**内部节点 num_key == 孩子数**，`key[0]` 为子树最小键哨兵。

#### (1) 结点内查找

```cpp
int IxNodeHandle::lower_bound(const char *target) const {   // 第一个 >= target 的下标
    int l=0, r=page_hdr->num_key;
    while (l<r) { int m=(l+r)/2;
        if (ix_compare(get_key(m),target,file_hdr->col_types_,file_hdr->col_lens_)<0) l=m+1; else r=m; }
    return l;
}
int IxNodeHandle::upper_bound(const char *target) const {   // 第一个 > target，从1开始（内部节点用）
    int l=1, r=page_hdr->num_key;
    while (l<r) { int m=(l+r)/2;
        if (ix_compare(get_key(m),target,...)<=0) l=m+1; else r=m; }
    return l;
}
page_id_t IxNodeHandle::internal_lookup(const char *key) {  // 下降到哪个孩子
    return value_at(upper_bound(key) - 1);
}
```

#### (2) 插入与分裂

```cpp
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *txn) {
    std::scoped_lock lock{root_latch_};
    auto [leaf, _] = find_leaf_page(key, Operation::INSERT, txn);
    int before = leaf->get_size();
    int after  = leaf->insert(key, value);          // 重复 key 不插入
    if (after == before) { unpin; return page_no; } // 唯一性由上层保证，这里只是不重复插
    if (leaf->get_size() >= leaf->get_max_size()) { // 满 → 分裂
        IxNodeHandle *new_node = split(leaf);
        if (file_hdr_->last_leaf_ == leaf->get_page_no()) file_hdr_->last_leaf_ = new_node->get_page_no();
        insert_into_parent(leaf, new_node->get_key(0), new_node, txn);  // 中间键上提，可能递归分裂
        unpin(new_node);
    }
    unpin(leaf); return leaf_page_no;
}
```

`split` 把结点右半部分搬到新结点，叶子还要维护 `prev_leaf/next_leaf` 双向链；`insert_into_parent` 处理根分裂（新建根）与递归向上分裂。

#### (3) 删除与合并/重分配

`delete_entry` → `coalesce_or_redistribute`：下溢时优先**重分配**（兄弟够借），否则**合并**。

**本实现的关键约定**（避免页删除重复释放）：
> `coalesce_or_redistribute` 不 unpin/delete 输入结点，由调用者负责；`coalesce` **始终把输入结点合并掉**（index==0 时把 node 前插到右兄弟、index>0 时追加到左兄弟），这样调用者要删的页永远是确定的 `node`。

```cpp
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, ...) {
    if (node->is_root_page()) return adjust_root(node);
    if (node->get_size() >= node->get_min_size()) return false;       // 未下溢
    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int index = parent->find_child(node);
    IxNodeHandle *neighbor = fetch_node(index==0 ? parent->value_at(1) : parent->value_at(index-1));
    if (node->get_size() + neighbor->get_size() >= node->get_min_size()*2) {
        redistribute(neighbor, node, parent, index);    // 借一个键值对
        unpin(parent); unpin(neighbor); return false;   // node 保留
    }
    bool parent_del = coalesce(&neighbor, &node, &parent, index, ...);  // 合并
    unpin(parent); if (parent_del) delete_page(parent); unpin(neighbor);
    return true;                                          // node 由调用者删除
}
```

#### (4) 树级 lower/upper_bound（索引扫描的边界）

```cpp
Iid IxIndexHandle::lower_bound(const char *key) {        // 第一个 >= key 的位置
    auto [leaf,_] = find_leaf_page(key, FIND, nullptr);
    int pos = leaf->lower_bound(key);
    Iid iid = (pos >= leaf->get_size())
            ? (leaf->get_page_no()==file_hdr_->last_leaf_ ? Iid{leaf->get_page_no(),pos}
                                                          : Iid{leaf->get_next_leaf(),0})
            : Iid{leaf->get_page_no(),pos};
    unpin(leaf); return iid;
}
```

### 4.5 索引扫描算子 — `executor_index_scan.h`

按最左匹配，从 where 条件构造 lower/upper key（等值前缀 + 末位范围，其余列填类型最小/最大值），用 IxScan 遍历区间并对**完整谓词**做残差过滤（保证正确性）：

```cpp
void beginTuple() override {
    auto ih = sm_manager_->ihs_.at(...).get();
    std::vector<char> low(tot), high(tot);
    bool low_inc=true, high_inc=true, prefix_open=true; int koff=0;
    for (auto &icol : index_meta_.cols) {
        set_min(low.data()+koff, icol.type, icol.len);   // 默认填最小/最大
        set_max(high.data()+koff, icol.type, icol.len);
        if (prefix_open) {
            // 找该列上的 = / > / >= / < / <= 条件
            if (has_eq)      { 写 low=high=val; }         // 等值：继续扩展前缀
            else if (range)  { 写 low/high；prefix_open=false; }  // 范围后停止
            else             { prefix_open=false; }       // 无条件停止
        }
        koff += icol.len;
    }
    Iid lower = low_inc ? ih->lower_bound(low.data()) : ih->upper_bound(low.data());
    Iid upper = high_inc ? ih->upper_bound(high.data()) : ih->lower_bound(high.data());
    scan_ = std::make_unique<IxScan>(ih, lower, upper, sm_manager_->get_bpm());
    find_next_valid();   // 残差过滤
}
```

### 4.6 最左匹配 — `planner.cpp`

```cpp
bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> curr_conds,
                             std::vector<std::string>& index_col_names) {
    TabMeta& tab = sm_manager_->db_.get_table(tab_name);
    for (auto& index : tab.indexes) {
        int matched = 0; bool stop = false;
        for (size_t i = 0; i < index.cols.size() && !stop; ++i) {
            // 在 curr_conds 里找该列的条件
            if (has_eq)         matched++;                // 等值 → 继续
            else if (has_range){ matched++; stop = true; }// 范围 → 停止扩展
            else                stop = true;              // 无条件 → 停止
        }
        if (matched > 0) {                                // 该索引可用，返回其完整列名
            for (auto& col : index.cols) index_col_names.push_back(col.name);
            return true;
        }
    }
    return false;
}
```

> 返回**完整索引列名**（让 `get_index_meta` 能找到索引），执行器再据条件构造可用前缀的区间。条件顺序无关（`name='x' and id=1` 与 `id=1 and name='x'` 都能匹配 `(id,name)` 索引）。

### 4.7 show index 与唯一性

`show_index` 输出（注意逗号无空格）：

```cpp
std::string cols_str = "(";
for (size_t i = 0; i < index.cols.size(); ++i) {
    if (i > 0) cols_str += ",";        // 不加空格
    cols_str += index.cols[i].name;
}
cols_str += ")";
outfile << "| " << tab_name << " | unique | " << cols_str << " |\n";
```

唯一性：insert 前对每个索引 `get_value(key)` 查重，存在则抛异常 → failure；update 做预检查（批内冲突 + 排除自身记录）。

### 4.8 测试

```sql
-- 测试点1：DDL + show
create table warehouse (id int, name char(8));
create index warehouse (id);
show index from warehouse;            -- | warehouse | unique | (id) |
create index warehouse (id,name);
show index from warehouse;            -- 两行
drop index warehouse (id);
drop index warehouse (id,name);
show index from warehouse;            -- 空

-- 测试点2：单点 + 范围 + 联合索引
create index warehouse(w_id);
select * from warehouse where w_id = 10;
select * from warehouse where w_id < 534 and w_id > 100;
create index warehouse(w_id,name);
select * from warehouse where w_id = 100 and name = 'qwerghjk';
select * from warehouse where w_id < 600 and name > 'bztyhnmj';   -- 前缀范围+残差过滤

-- 测试点3：维护 + 唯一性
create index warehouse(w_id);
insert into warehouse values (500, 'lastdanc');
update warehouse set w_id = 507 where w_id = 534;   -- 删旧键插新键
```

> 题目说明测试点2、3不要求行序与期望完全一致（列序要求一致）。

---

## 5. 聚合函数

### 5.1 题目分析

实现 COUNT/MAX/MIN/SUM，支持 `AS` 别名：
- COUNT/MAX/MIN 支持 int/float/char；SUM 只支持 int/float。
- SUM 浮点保留 6 位小数、整数不显示小数；表头与 SQL 的 AS 别名一致。
- 支持 `COUNT(*)` 和 `COUNT(col)`。

### 5.2 设计核心

在扫描算子之上加一层**聚合算子**：扫描负责 where 过滤，聚合算子消费全部元组算出单一值，输出一行。结果类型决定输出格式（int→无小数、float→6 位小数）。

### 5.3 涉及改动

| 层 | 文件 | 改动 |
|----|------|------|
| 类型 | [src/defs.h](src/defs.h) | `enum AggType { AGG_NONE, AGG_COUNT, AGG_MAX, AGG_MIN, AGG_SUM }` |
| 列结构 | [common.h](src/common/common.h) `TabCol`、[ast.h](src/parser/ast.h) `Col` | 加 `agg_type`/`alias`/`is_star` |
| 词法/语法 | [lex.l](src/parser/lex.l)、[yacc.y](src/parser/yacc.y) | `COUNT/SUM/MAX/MIN/AS` 关键字；`selectCol` 规则 + 可选 `as_clause` |
| 语义 | [analyze.cpp](src/analyze/analyze.cpp) | 传递聚合字段，`COUNT(*)` 跳过列解析 |
| 计划 | [plan.h](src/optimizer/plan.h) `AggregatePlan`、[planner.cpp](src/optimizer/planner.cpp) | 含聚合用聚合算子，否则投影 |
| 算子 | [executor_aggregate.h](src/execution/executor_aggregate.h)（新） | 计算聚合，输出单行 |
| 接线/输出 | [portal.h](src/portal.h)、[execution_manager.cpp](src/execution/execution_manager.cpp) | AggregatePlan 转算子；表头用别名 |

### 5.4 关键代码解析

#### (1) 语法 — `yacc.y`

```
selectCol:
    col                              { $$ = $1; }                  // 普通列
  | COUNT '(' '*' ')' as_clause      { 构造 Col("*"), agg=COUNT, is_star=true, alias=$5 }
  | COUNT '(' col ')' as_clause      { $3->agg_type=AGG_COUNT; $3->alias=$5; $$=$3; }
  | SUM   '(' col ')' as_clause      { ... AGG_SUM ... }
  | MAX   '(' col ')' as_clause      { ... AGG_MAX ... }
  | MIN   '(' col ')' as_clause      { ... AGG_MIN ... }
  ;
as_clause: /* empty */ { $$=""; } | AS colName { $$=$2; } ;
```

#### (2) 聚合算子 — `executor_aggregate.h`

```cpp
void beginTuple() override {
    std::vector<long long> cnt(n,0); std::vector<double> sum(n,0);
    std::vector<bool> has_val(n,false); std::vector<std::vector<char>> minmax(n);
    for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
        auto rec = prev_->Next();
        for (int i=0;i<n;i++) {
            auto &sc = sel_cols_[i];
            if (sc.agg_type==AGG_COUNT) { cnt[i]++; continue; }    // COUNT(*) / COUNT(col)
            auto pos = get_col(child_cols, {sc.tab_name, sc.col_name});
            char *data = rec->data + pos->offset;
            if (sc.agg_type==AGG_SUM) { sum[i] += (pos->type==TYPE_INT? *(int*)data : *(float*)data); }
            else { // MAX/MIN：用 compare_value 维护极值
                if (!has_val[i]) { minmax[i].assign(data,data+pos->len); has_val[i]=true; }
                else { int c=compare_value(data,minmax[i].data(),pos->type,pos->len);
                       if ((sc.agg_type==AGG_MAX&&c>0)||(sc.agg_type==AGG_MIN&&c<0))
                           minmax[i].assign(data,data+pos->len); }
            }
        }
    }
    // 写结果：COUNT/SUM→int或float，MAX/MIN→原类型
    is_end_ = false;
}
void nextTuple() override { is_end_ = true; }   // 聚合只有一行
```

#### (3) 表头用别名 — `execution_manager.cpp`

```cpp
for (auto &sel_col : sel_cols) {
    captions.push_back(sel_col.alias.empty() ? sel_col.col_name : sel_col.alias);  // 普通列别名为空→沿用列名
}
```

### 5.5 测试

```sql
create table aggregate (id int,val float);
insert into aggregate values(1,5.5);
insert into aggregate values(3,4.5);
insert into aggregate values(5,10.0);
select SUM(id) as sum_id from aggregate;     -- | sum_id | / | 9 |
select SUM(val) as sum_val from aggregate;   -- | 20.000000 |
select MAX(id) as max_id from aggregate;     -- | 5 |
select MIN(val) as min_val from aggregate;   -- | 4.500000 |
select COUNT(*) as count_row from aggregate; -- | 3 |
select COUNT(name) as count_name from aggregate where val = 2.0;  -- where 由扫描处理
```

---

## 6. ORDER BY + LIMIT

### 6.1 题目分析

支持 `ORDER BY col [ASC|DESC], ...`（多列、默认升序）和 `LIMIT n`（限制结果集大小）。

### 6.2 设计核心

算子树：`扫描 → 排序 → 投影 → LIMIT`。排序算子物化全部记录后按多列键**稳定排序**；LIMIT 算子在最外层截断行数。

### 6.3 涉及改动

| 层 | 文件 | 改动 |
|----|------|------|
| AST | [ast.h](src/parser/ast.h) | `OrderBy` 改多列(`cols`+`dirs`)；`SelectStmt` 加 `int limit` |
| 词法/语法 | [lex.l](src/parser/lex.l)、[yacc.y](src/parser/yacc.y) | `LIMIT` 关键字；`order_clause` 逗号多列；`opt_limit` |
| 计划 | [plan.h](src/optimizer/plan.h) | `SortPlan` 改多列；新增 `LimitPlan` |
| 优化器 | [planner.cpp](src/optimizer/planner.cpp) | `generate_sort_plan` 多列；末尾按 limit 包 `LimitPlan` |
| 算子 | [execution_sort.h](src/execution/execution_sort.h)、[executor_limit.h](src/execution/executor_limit.h)（新） | 多键排序、LIMIT |
| 接线 | [portal.h](src/portal.h) | SortPlan→SortExecutor（多列）；T_select 拆 LimitPlan |

### 6.4 关键代码解析

#### (1) 多键排序 — `execution_sort.h`

```cpp
void beginTuple() override {
    tuples_.clear();
    for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple())
        tuples_.push_back(prev_->Next());                 // 物化
    std::stable_sort(tuples_.begin(), tuples_.end(),      // 稳定排序保证等值行原序
        [this](const auto &a, const auto &b){ return compare_tuple(a.get(), b.get()) < 0; });
    pos_ = 0;
}
int compare_tuple(const RmRecord *a, const RmRecord *b) {
    for (size_t i=0;i<order_cols_.size();++i) {           // 逐列比较
        auto &col = order_cols_[i];
        int c = compare_value(a->data+col.offset, b->data+col.offset, col.type, col.len);
        if (c != 0) return is_desc_[i] ? -c : c;          // 降序取负
    }
    return 0;
}
```

#### (2) LIMIT 算子 — `executor_limit.h`

```cpp
void beginTuple() override { count_=0; prev_->beginTuple(); }
void nextTuple()  override { count_++; prev_->nextTuple(); }
bool is_end() const override { return count_ >= limit_ || prev_->is_end(); }   // 达到 limit 即结束
```

#### (3) 接线 — `portal.h`（T_select 拆出 LimitPlan）

```cpp
std::shared_ptr<Plan> sub = x->subplan_;
int limit = -1;
if (auto l = std::dynamic_pointer_cast<LimitPlan>(sub)) { limit = l->limit_; sub = l->subplan_; }
// 从 sub(投影/聚合) 取 sel_cols
auto root = convert_plan_executor(sub, context);
if (limit >= 0) root = std::make_unique<LimitExecutor>(std::move(root), limit);
```

### 6.5 测试

```sql
create table orders (company char(10), order_number int);
insert into orders values('AAA',12); insert into orders values('ABB',13);
insert into orders values('ABC',19); insert into orders values('ACA',1);
SELECT company, order_number FROM orders ORDER BY order_number;                 -- 升序
SELECT company, order_number FROM orders ORDER BY company, order_number;        -- 多列
SELECT company, order_number FROM orders ORDER BY company DESC, order_number ASC;  -- 混合方向
SELECT company, order_number FROM orders ORDER BY order_number ASC LIMIT 2;     -- LIMIT
```

---

## 7. 基础设施 Bug 修复

这两个 bug 不在题目要求内，但不修系统跑不起来/索引出错。

### 7.1 事务层 begin/commit/abort（题目二时修）

**现象**：框架的 `TransactionManager::begin` 返回 `nullptr`，导致 `rmdb.cpp::SetTransaction` 对空事务调 `get_transaction_id()` → **段错误**，任何 SQL 都崩。

**修复**（[transaction_manager.cpp](src/transaction/transaction_manager.cpp)）：实现最小可用的单语句事务。

```cpp
Transaction* TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    std::unique_lock<std::mutex> lock(latch_);
    if (txn == nullptr) {
        txn_id_t new_txn_id = next_txn_id_++;
        txn = new Transaction(new_txn_id);
        txn->set_start_ts(next_timestamp_++);
    }
    txn->set_state(TransactionState::DEFAULT);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}
// commit：释放锁、清写集、置 COMMITTED
// abort：逆序回滚写集(INSERT→delete, DELETE→insert, UPDATE→还原)、释放锁、置 ABORTED
```

### 7.2 缓冲池 fd 复用脏页（B+树题时修）

**现象**：`drop index` 关闭索引文件后，OS 复用了该 fd 号给下一个新建索引；但缓冲池仍残留旧索引的页（key 是 `{fd, page_no}`），新索引读到**旧文件的脏缓存** → 查询 `failure`。在「连续 create/drop index」时触发。

**修复**：新增 [`BufferPoolManager::delete_all_pages(int fd)`](src/storage/buffer_pool_manager.cpp)，关闭文件时落盘并把对应帧回收：

```cpp
void BufferPoolManager::delete_all_pages(int fd) {
    std::scoped_lock lock{latch_};
    for (size_t i = 0; i < pool_size_; i++) {
        Page *page = &pages_[i];
        if (page->get_page_id().fd == fd && page->get_page_id().page_no != INVALID_PAGE_ID) {
            if (page->is_dirty_) { disk_manager_->write_page(...); page->is_dirty_=false; }
            page_table_.erase(page->get_page_id());     // 从页表移除
            page->id_ = {-1, INVALID_PAGE_ID};
            page->pin_count_ = 0;
            page->reset_memory();
            free_list_.push_back((frame_id_t)i);         // 帧归还空闲链
        }
    }
}
```

并在 [ix_manager.h](src/index/ix_manager.h) `close_index` 和 [rm_manager.h](src/record/rm_manager.h) `close_file` 中用它替换原来的 `flush_all_pages`。

---

## 8. 验收 Q&A

> 以下为答辩/验收时可能被问到的问题与参考答复。

### 8.1 总体与架构

**Q：一条 SELECT 从输入到输出经过哪些阶段？**
A：词法/语法解析生成 AST → Analyze 语义分析生成 Query（检查表/列存在性、类型、隐式转换）→ Optimizer/Planner 生成查询计划树（扫描/连接/排序/投影/聚合/limit）→ Portal 把计划转成算子树 → 火山模型逐 tuple 执行 → select_from 格式化写入 output.txt 并返回客户端。

**Q：算子是怎么组织的（执行模型）？**
A：火山模型（Volcano/Iterator）。每个算子实现 `beginTuple/nextTuple/is_end/Next/cols`。上层算子通过反复调用下层的这组接口拉取数据，形成流水线。

**Q：为什么把新类型（BIGINT/DATETIME）放在 ColType 枚举末尾？**
A：元数据 `db.meta` 把 ColType 当 int 序列化。放末尾不改变已有类型（INT=0,FLOAT=1,STRING=2）的值，保证旧库元数据兼容。

### 8.2 题目二（查询执行）

**Q：failure 是怎么产生的？语法错误也会 failure 吗？**
A：执行链路任意一层抛 `RMDBError`（建已存在表、删不存在表、where 用不存在的列、类型不兼容等），被 rmdb.cpp 的 `catch(RMDBError)` 捕获后写 `failure`。**注意**：纯语法解析失败（yyparse≠0）不会写 failure，所以像「数值越界」这类要在语义/执行阶段抛异常，而不是让解析失败。

**Q：连接查询的行序是怎么保证和期望一致的？**
A：planner 对无谓词笛卡尔积生成的计划是 `left=后表,right=前表`。我让嵌套循环以**右表为外层、左表物化后逆序为内层**，这样 `select * from t,d` 与带谓词连接两条查询的输出行序都与期望逐字节一致。题目本身对连接行序其实不强制，但这样能稳过 exact diff。

**Q：浮点精度怎么处理？**
A：输出用 `std::to_string(float)`，默认 6 位小数；整数用 `std::to_string(int)` 无小数。比较时 `score>90` 把 int 90 隐式转 float 再比，避免类型不兼容。

### 8.3 BIGINT / DATETIME

**Q：BIGINT 怎么和 INT 区分？小整数插入 BIGINT 列会怎样？**
A：词法层对整型字面量先 `strtoll`，落在 int32 范围内当 `VALUE_INT`，否则 `VALUE_BIGINT`。小整数（INT）插入 BIGINT 列时，`Value::cast_to` 做 int→bigint 隐式提升。

**Q：9223372036854775809 为什么是 failure？**
A：它超出 int64 上限，`strtoll` 置 `errno=ERANGE`，`BigintLit.overflow=true`，语义层 `convert_sv_value` 抛 RMDBError → failure。失败发生在执行写入前，不影响已有数据。

**Q：DATETIME 为什么不用字符串存？8 字节怎么放下 19 个字符？**
A：放不下，所以编码成 int64 `YYYYMMDDHHMMSS`（最大 99991231235959 < int64 上限）。好处：① 8 字节定长；② 该编码**单调对应时间先后**，比较/相等直接用 int64 比较，复用已有比较逻辑；③ 输出时格式化回字符串。

**Q：DATETIME 合法性都校验了哪些？**
A：长度严格 19、分隔符位置、各位是数字（排除负号）、年∈[1000,9999]、月∈[1,12]、时≤23、分秒≤59、按月份天数校验日（含闰年 2 月 29 天）。任一不满足返回 false → failure。

### 8.4 B+ 树索引（重点）

**Q：B+ 树的节点结构？内部节点和叶子节点有什么区别？**
A：每页 = 页头 + keys 数组 + rids 数组。内部节点的 `rid.page_no` 指向孩子页，`key[i]` 是孩子 i 子树的最小键；叶子节点的 rid 指向基表记录位置，并维护 `prev_leaf/next_leaf` 双向链表用于范围扫描。

**Q：插入导致分裂时，键是怎么向上传播的？**
A：叶子满了就 split 成左右两半，把**新结点的第一个 key** 插到父节点（`insert_into_parent`）。若父节点也满则递归分裂；若分裂的是根，则新建根，树高 +1。

**Q：删除时的合并和重分配怎么选？**
A：结点删除后若 `size < min_size`（下溢），看兄弟：兄弟键够借（两者之和 ≥ 2*min）就**重分配**借一个键值对；否则**合并**两结点。合并会减少父节点一个 key，可能递归触发父节点下溢处理。

**Q：合并时页面释放怎么避免重复 delete？**
A：我定的约定是 `coalesce_or_redistribute` 不负责删自己（输入结点），由调用者删；`coalesce` 始终把输入结点合并掉（前插到右兄弟或追加到左兄弟），这样「要删的页」永远是确定的输入结点，调用者删一次即可，不会重复释放。

**Q：最左匹配怎么实现的？为什么能自动调整 where 顺序？**
A：`get_index_cols` 遍历表上每个索引，从第一列起检查 where 是否覆盖该列（等值则继续向后，遇范围则停止扩展，无条件则停止）。只要前缀匹配到至少一列就用该索引。因为是「按索引列顺序去 where 里找条件」，而不是「按 where 顺序」，所以 `name='x' and id=1` 和 `id=1 and name='x'` 都能匹配 `(id,name)`。

**Q：范围查询是怎么用索引的？怎么保证正确性？**
A：扫描算子根据条件构造 lower/upper 两个 key（等值前缀照填，末位范围列填边界，其余列填类型 min/max），用树级 `lower_bound/upper_bound` 定位区间端点，IxScan 沿叶子链遍历该区间；同时对每条记录再用**完整谓词**做残差过滤，保证即使边界放宽也结果正确。

**Q：怎么证明「真正用了索引」（性能要求 <70%）？**
A：索引扫描是 O(log n) 定位 + narrow 区间遍历，而 SeqScan 是 O(n) 全表。我用 5000 行做过压力测试：点查/范围查只遍历命中区间。planner 在 `get_index_cols` 匹配到索引时生成 `T_IndexScan`，否则 `T_SeqScan`。

**Q：唯一性约束在哪里检查？**
A：insert 在写记录前对每个索引 `get_value(key)` 查重，存在即抛异常 → failure；update 做预检查（计算新 key，检查批内冲突 + 是否与非本批记录冲突，排除自身）。

**Q：show index 的输出格式？**
A：`| 表名 | unique | (列1,列2) |`，**多列之间逗号后不加空格**。遍历 `tab.indexes` 逐个输出。

### 8.5 聚合 / ORDER BY / LIMIT

**Q：聚合算子放在算子树的哪一层？where 怎么处理？**
A：放在扫描之上：`扫描(带where过滤) → 聚合`。where 由下层扫描算子处理，聚合只对过滤后的元组做计算，输出一行。

**Q：SUM(int) 和 SUM(float) 输出有什么不同？怎么实现的？**
A：内部用 double 累加，输出时按结果类型回写：源是 int → 结果 int（输出 `9`，无小数）；源是 float → 结果 float（输出 `20.000000`）。结果类型在算子构造时由源列类型决定。

**Q：MAX/MIN 支持哪些类型？怎么比较？**
A：int/float/char 都支持，复用 `compare_value`（char 用 memcmp）。维护一个「当前极值的原始字节」，逐行比较更新。

**Q：表头的别名（AS）怎么显示出来的？**
A：`select_from` 生成表头时，列有别名就用别名，否则用列名（普通列别名为空，行为不变）。

**Q：ORDER BY 多列、混合升降序怎么实现？**
A：Sort 算子物化全部记录，`std::stable_sort` + 逐列 `compare_value`；某列降序就把该列比较结果取负。前一列相等才比下一列。用稳定排序保证等值行保持原插入顺序。

**Q：ORDER BY 能按未出现在 select 列表的列排序吗？**
A：能。排序算子在投影**之下**、扫描之上，操作的是扫描输出的完整表记录，可按任意表字段排序；排好序后再由上层投影裁剪列。

**Q：LIMIT 怎么实现？能不带 ORDER BY 单独用吗？**
A：独立的 LimitExecutor，计数到 limit 即 `is_end`。可与/不与 ORDER BY 组合（计划里 LimitPlan 包在最外层）。

### 8.6 工程 / 调试类

**Q：为什么改了 lex.l/yacc.y 要重新生成？怎么生成的？**
A：CMake 配置了 flex/bison 目标，`make` 时若 .l/.y 比生成的 .cpp 新就自动用 flex/bison 重新生成 `lex.yy.cpp`/`yacc.tab.cpp/.h`，不需手动跑。

**Q：测试时遇到过什么坑？**
A：① 残留服务端占端口 8765，新服务端 Bind error，客户端连到旧进程 → 每次先 `pkill -9 rmdb`；② output.txt 追加写，复测要删库；③ 只有 select/show/failure 写 output.txt。④ fd 复用脏页 bug（见 7.2）。

**Q：output.txt 在哪？为什么有时找不到？**
A：服务端 `open_db` 时 chdir 进库目录，所以在 `build/<库名>/output.txt`。找不到通常是：只跑了 create/insert（不写输出）、或连到了别的库的旧服务端。

---

## 附：一键测试脚本 build/run_test.sh

```bash
#!/bin/bash
# 用法: run_test.sh <db_name> <sql_file>
BUILD_DIR="$(cd "$(dirname "$0")" && pwd)"; cd "$BUILD_DIR"
DB="$1"; SQL="$2"; CLIENT="$BUILD_DIR/../rmdb_client/build/rmdb_client"
rm -rf "$DB"
./bin/rmdb "$DB" >/tmp/server_${DB}.log 2>&1 &
SRV=$!
for i in $(seq 1 50); do grep -q "Waiting for new connection" /tmp/server_${DB}.log 2>/dev/null && break; sleep 0.1; done
sleep 0.3
(cat "$SQL"; echo "exit") | "$CLIENT" >/tmp/client_${DB}.log 2>&1 || true
sleep 0.3; kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
echo "===== output.txt for $DB ====="; cat "$DB/output.txt" 2>/dev/null || echo "(no output.txt)"
```

> 注：该脚本未自动 `pkill`，若有残留服务端仍会冲突，建议运行前手动 `pkill -9 rmdb`。
