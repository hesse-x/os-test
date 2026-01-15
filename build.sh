#!/bin/bash
# 1. 编译汇编入口文件boot.s，生成目标文件boot.o
nasm -f elf32 boot.s -o boot.o

# 2. 编译C语言内核文件kernel.c，生成目标文件kernel.o（裸机编译，无libc依赖）
gcc -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -c kernel.c -o kernel.o

# 3. 链接目标文件，生成ELF内核镜像myos.bin
ld -m elf_i386 -T linker.ld boot.o kernel.o -o myos.bin

# 4. 验证：检查myos.bin是否为有效ELF镜像（可选）
file myos.bin

# 1. 创建临时目录结构（GRUB2要求的目录格式）
mkdir -p iso/boot/grub

# 2. 将内核镜像myos.bin复制到iso/boot目录下
cp myos.bin iso/boot/myos.bin

# 3. 编写GRUB2配置文件（iso/boot/grub/grub.cfg）
cat > iso/boot/grub/grub.cfg << EOF
# GRUB2配置文件：定义启动菜单和内核路径
set default=0
set timeout=5

menuentry "My Operating System (GRUB2)" {
    set gfxpayload=text
    insmod efi_gop
    insmod efi_uga
    insmod multiboot2
    # 加载内核镜像：指定路径和Multiboot规范
    multiboot /boot/myos.bin
    boot
}
EOF

# 4. 制作ISO镜像（myos.iso），集成GRUB2引导程序
grub-mkrescue -o myos.iso iso

# 5. 清理临时目录（可选）
rm -rf iso 
