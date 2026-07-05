# 代码格式化方案

## 风格规则

- **C 代码**：GNU + `BreakBeforeBraces: Attach`（全 snake_case，大括号不换行）
- **C++ 代码**：LLVM 风格

## 目录划分

| 目录 | 语言 | 格式化风格 |
|------|------|-----------|
| `arch/` | C | GNU + Attach |
| `kernel/` | C | GNU + Attach |
| `common/` | C | GNU + Attach |
| `init/` | C | GNU + Attach |
| `user/` | C++ | LLVM |

## 待办

- [ ] 更新 `format.sh` 按目录应用不同风格
