#!/bin/bash
# 构建期断言：ld.so 的 .rela.plt 必须为空
# 用法：verify_ldso_rela_plt.sh <ldso_elf>
# 失败（含 JUMP_SLOT 重定位）→ exit 1
# ld.so 全局 -fvisibility=hidden 后，内部函数不走 PLT，.rela.plt 应为空
# 若非空，说明有内部函数未标 hidden，bootstrap 前 GOT 未填会跳 lazy stub 崩溃
elf="$1"
if readelf -r "$elf" 2>/dev/null | grep 'R_X86_64_JUMP_SLO'; then
    echo "FATAL: $elf has JUMP_SLOT relocations in .rela.plt"
    echo "  ld.so internal functions must be hidden visibility"
    echo "  (add -fvisibility=hidden to add_user_ldso COMPILE_FLAGS)"
    echo "  PLT calls before GOT is filled jump to lazy stub and crash"
    exit 1
fi
