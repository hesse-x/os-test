# 定义Multiboot2相关常量（来自Multiboot2规范）
.set MULTIBOOT2_MAGIC,  0xE85250D6  # Multiboot2魔术值（固定）
.set MULTIBOOT2_ARCH,   0x00000000  # 架构标识：0表示i386 32位保护模式
.set MULTIBOOT2_HEADER_LEN, multiboot2_header_end - multiboot2_header  # 头部长度
.set MULTIBOOT2_CHECKSUM, -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_HEADER_LEN)  # 校验和

# -------------------------- Multiboot2 头部（必须在前8192字节内，且32位对齐）--------------------------
.section .multiboot, "a", @progbits  # 定义multiboot段，ALLOC属性，程序数据
.align 4  # 32位（4字节）对齐，符合Multiboot2要求

multiboot2_header:
    # Multiboot2头部核心字段（必须按此顺序）
    .long MULTIBOOT2_MAGIC    # 魔术值（小端序，32位）
    .long MULTIBOOT2_ARCH     # 架构标识
    .long MULTIBOOT2_HEADER_LEN  # 头部总长度
    .long MULTIBOOT2_CHECKSUM # 校验和：四个字段之和模2^32等于0

    # 可选：添加终止标签（Multiboot2要求头部以终止标签结尾）
    .short 0x0000  # 标签类型：0表示终止标签
    .short 0x0000  # 标签标志：无特殊标志
    .long  0x00000008  # 标签长度：8字节（标签类型+标志+长度）
multiboot2_header_end:

# -------------------------- BSS 段（未初始化数据，用于分配栈空间）--------------------------
.align 16  # 16字节对齐（符合System V ABI，保证栈对齐）
stack_bottom:
    .skip 8192  # 分配8KB栈空间（向下生长，栈顶在stack_top）
stack_top:

# -------------------------- 入口点（引导加载器跳转至此执行）--------------------------
.section .text, "ax", @progbits  # 定义代码段，ALLOC+EXEC属性
.global _start  # 导出_start符号，作为链接入口点
.type _start, @function  # 标记为函数类型

_start:
    # 此时CPU已被Multiboot2引导加载器切换到32位保护模式：
    # - 分段机制已启用，段寄存器被设置为平坦模式（段基址0，限长4GB）
    # - 分页机制未启用
    # - 中断已禁用
    # - 栈尚未初始化（需要我们自己设置栈指针）

    # 1. 初始化栈指针（指向栈顶，x86栈向下生长）
    movl $stack_top, %esp  # 将栈顶地址加载到ESP寄存器

    # 2. （可选）保存Multiboot2传递的参数（供后续C代码使用）
    # Multiboot2引导加载器会传递两个参数到_start：
    # - %ebx：Multiboot2信息结构的物理地址
    # - %eax：MULTIBOOT2_MAGIC（验证引导有效性）
    pushl %ebx  # 保存Multiboot2信息结构指针
    pushl %eax  # 保存魔术值

    # 3. （可选）调用内核主函数（如果有后续C语言内核）
    call kernel_main  # 假设存在C语言内核入口函数kernel_main

    # 4. 内核返回后的处理：进入无限空闲循环（防止程序退出）
    cli  # 禁用中断（关闭所有可屏蔽中断）
1:  hlt  # 暂停CPU，等待中断唤醒（无中断时将一直暂停）
    jmp 1b  # 跳转到上面的hlt指令，形成无限循环

# 定义_start函数的长度（GAS规范）
.size _start, . - _start
