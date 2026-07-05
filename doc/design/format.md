# 代码格式化方案

## 总则

- **格式化**:全部目录统一 **LLVM 风格**(2 空格缩进、大括号不换行、列宽 80)。仓库根目录单一 `.clang-format`(`BasedOnStyle: LLVM`),所有目录共用,无需按目录分发。
- **命名**:格式由 clang-format 保证;命名由 clang-tidy `readability-identifier-naming` **增量强制**(只查 diff,不整改存量),code review 兜底语义。

## 命名约定

### C 代码(`arch/` `kernel/` `common/` `init/` `driver/` `shell/`)

遵循 Linux 内核命名约定,与 POSIX 接口一致:

- 函数 / 变量 / 类型(struct/union/enum):`snake_case`(如 `sys_open`、`nr_threads`、`task_struct`)
- 宏 / 常量:`SCREAMING_SNAKE_CASE`(如 `PAGE_SIZE`、`GFP_KERNEL`)
- 类型 typedef 保留 `_t` 后缀(如 `xtask_t`、`bsd_proc_t`、`spinlock_t`),贴合 POSIX 且与现有代码一致

### C++ 代码(`user/`)

LLVM C++ 命名约定:

- 类型 / 类 / 函数 / 方法:`CamelCase`
- 变量 / 成员:`lowerCamelCase`

## 目录划分

| 目录 | 语言 | 格式 | 命名 |
|------|------|------|------|
| `arch/` `kernel/` `common/` `init/` `driver/` `shell/` | C | LLVM | Linux snake_case + `_t` |
| `user/` | C++ | LLVM | LLVM CamelCase |

## 命名检查(clang-tidy,增量)

### 前提

- CMake 导出编译数据库:`set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`(根 CMakeLists.txt),产出 `build/compile_commands.json`。
- 安装 clang-tidy(本机当前未装;工具链为 gcc,clang-tidy 仅做语义解析不链接,需实测 `-ffreestanding -nostdlib -fPIE -mno-red-zone -std=gnu17` 及自定义 `-I` 的误报率)。

### 策略

- **只查增量,不整改存量**:CI/hook 仅对 `git diff` 触及的文件/行跑 clang-tidy,历史代码不背锅,规避 `--fix` 的语义改写风险。
- **配置按目录分发**:仓库根 `.clang-tidy` 管 C 代码规则;`user/.clang-tidy` 覆盖 C++ 规则(clang-tidy 按目录向上查找,与 clang-format 同机制)。
- **不进实时构建**:identifier-naming 需语义分析,比 clang-format 慢一两个量级,只作提交前/CI 检查,不挂进 `build.sh`。

### `.clang-tidy` 规则(C,根目录)

```yaml
Checks: 'readability-identifier-naming'
HeaderFilterRegex: '.*'
CheckOptions:
  - { key: readability-identifier-naming.FunctionCase,        value: lower_case }
  - { key: readability-identifier-naming.VariableCase,        value: lower_case }
  - { key: readability-identifier-naming.ParameterCase,       value: lower_case }
  - { key: readability-identifier-naming.StructCase,          value: lower_case }
  - { key: readability-identifier-naming.UnionCase,           value: lower_case }
  - { key: readability-identifier-naming.EnumCase,            value: lower_case }
  - { key: readability-identifier-naming.EnumConstantCase,    value: UPPER_CASE }
  - { key: readability-identifier-naming.MacroDefinitionCase, value: UPPER_CASE }
  - { key: readability-identifier-naming.TypedefSuffix,       value: _t }
  - { key: readability-identifier-naming.TypeAliasSuffix,     value: _t }
```

> **常量**:不设 `ConstantCase`——枚举常量、宏走 `UPPER_CASE`,其余常量(`const` 变量)随 `VariableCase`(`lower_case`)。

### `.clang-tidy` 规则(C++,`user/`)

```yaml
Checks: 'readability-identifier-naming'
HeaderFilterRegex: '.*'
CheckOptions:
  - { key: readability-identifier-naming.ClassCase,           value: CamelCase }
  - { key: readability-identifier-naming.StructCase,          value: CamelCase }
  - { key: readability-identifier-naming.EnumCase,            value: CamelCase }
  - { key: readability-identifier-naming.FunctionCase,        value: CamelCase }
  - { key: readability-identifier-naming.MethodCase,          value: CamelCase }
  - { key: readability-identifier-naming.VariableCase,        value: camelBack }
  - { key: readability-identifier-naming.EnumConstantCase,    value: UPPER_CASE }
  - { key: readability-identifier-naming.MacroDefinitionCase, value: UPPER_CASE }
```

> **私有成员后缀(约定,非 clang-tidy 规则)**:私有成员变量**仅在与成员函数同名时**加 `_` 后缀消歧(如 `size_` 与 `size()`),其余不加。clang-tidy 的 `PrivateMemberSuffix` 是"所有私有成员统一加后缀",做不了条件判断,故此条靠 code review 兜底。
> **常量**:按 LLVM 规范,不设 `ConstantCase`——枚举常量、宏走 `UPPER_CASE`,其余常量(`const` 变量)随 `VariableCase`(`camelBack`)。

### 增量运行

```bash
# 只检查本次改动触及的文件(需先 cmake 生成 compile_commands.json)
git diff --name-only --diff-filter=d origin/master...HEAD -- '*.c' '*.h' \
  | run-clang-tidy -p build/compile_commands.json
```

## 待办

无（目录重组 `driver/`→`user/driver/`、`shell/`→`user/shell/` 已完成；全目录统一 LLVM 风格，无需按目录分发的 `format.sh`）。
