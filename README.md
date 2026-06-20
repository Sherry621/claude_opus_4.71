## 2026年全国大学生计算机系统能力大赛-数据库管理系统设计赛

## 快速上手 (Quick Start)

为确保团队成员在克隆本项目后能够顺利编译运行，请按照以下说明在 WSL2 (Ubuntu 24.04) 环境下配置和运行项目。

### 1. 环境依赖配置

在开始之前，必须安装核心依赖工具：CMake 构建工具、GCC (支持 C++17) 以及 SQL 解析器所需的 Flex 和 Bison。
在 WSL2 (Ubuntu) 终端中执行以下命令：

```bash
sudo apt-get update
sudo apt-get install -y gcc g++ cmake flex bison libreadline-dev
```
> **注**：`libreadline-dev` 是为了方便后续构建命令行交互式终端，提前安装可避免头文件找不到的错误。

### 2. 构建项目

我们约定统一使用相对路径 `rmdb` 进行开发。克隆项目后，进入 `rmdb` 目录并创建构建目录：

```bash
# 1. 进入核心项目目录
cd rmdb

# 2. 创建并进入 build 目录，防止编译生成的中间文件污染源代码
mkdir -p build
cd build

# 3. 生成 Makefile 并编译
cmake ..
make -j4
```

### 3. 运行测试

编译成功后，单测可执行文件会被输出到 `bin` 目录下。您可以在 `build` 目录下直接运行单元测试，以验证您的本地环境配置是否成功：

```bash
../bin/unit_test
```
如果看到 `[  PASSED  ]` 字样，说明核心存储与记录模块运行正常！

---
> **关于代码中的绝对路径疑虑**：
> 在编译过程中，`flex` 和 `bison` 会根据本机的绝对路径自动生成 `lex.yy.cpp` 和 `yacc.tab.cpp`，这些文件中会带有本机的绝对路径（如 `#line ... "/home/.../"`）。请放心，这些是编译时的**自动生成文件**，并不是硬编码上去的。不同成员克隆并在自己电脑上重新 `make` 后，会自动生成对应自己本机环境的绝对路径。为保持代码库整洁，您可以选择不将这些生成文件 push 到远程仓库中。
