# crt0.o — _start 入口（静态/动态主 ELF 共用）
# ld.md §3.7.2 / plan_ld2b3 T4
# _start 不在 libc.a/libc.so 中，独立产物，链入每个主 ELF

.section .text
.global _start
_start:
    # 栈布局：[rsp]=argc, [rsp+8]=argv[0], ..., argv[argc]=NULL, envp, ..., auxv
    # 参数映射（自定义 ABI，复用 SysV 前 6 寄存器 + 栈传第 7 个）：
    #   rdi = main, rsi = argc, rdx = argv,
    #   rcx = init_array_start, r8 = init_array_end,
    #   r9  = fini_array_start, [rsp+8] = fini_array_end
    # init_array/fini_array 范围由 crt0 从主 ELF 链接器符号取址传入，
    # 避免 libc.so 跨对象引用 hidden 符号 __init_array_start/end
    leaq main(%rip), %rdi        # main 函数地址
    movq (%rsp), %rsi            # argc
    leaq 8(%rsp), %rdx           # argv
    leaq __init_array_start(%rip), %rcx
    leaq __init_array_end(%rip), %r8
    leaq __fini_array_start(%rip), %r9
    # 16 字节对齐栈（x86-64 SysV ABI：call 时 rsp%16==0，被调用者入口 rsp%16==8）
    # 第 7 参数 fini_array_end 通过栈传：sub $16 保持对齐，mov 到 (%rsp)
    subq $16, %rsp
    leaq __fini_array_end(%rip), %rax
    movq %rax, (%rsp)
    call __libc_start_main
1:  hlt
    jmp 1b

    .section .note.GNU-stack,"",@progbits