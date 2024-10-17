C_SOURCES = $(wildcard kernel/*.c drivers/*.c cpu/*.c libc/*.c)
HEADERS = $(wildcard kernel/*.h drivers/*.h cpu/*.h libc/*.h)
# Nice syntax for file extension replacement
OBJ = ${C_SOURCES:.c=.o cpu/interrupt.o} 

# Change this if your cross-compiler is somewhere else
# CC=$(HOME)/llvm-project/build/bin/clang
# LD=$(HOME)/llvm-project/build/bin/ld.lld
CC=gcc
LD=ld
# -g: Use debugging symbols in gcc
CFLAGS = -g -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nodefaultlibs -fno-pic -Wall -Wextra -Werror -mno-sse

# First rule is run by default
os-image.bin: bootsect.bin kernel.bin
	cat $^ > os-image.bin

bootsect.o: boot/bootsect.s
	$(CC) $< -c -o $@

bootsect.bin: bootsect.o
	$(LD) -o $@ -Ttext 0x7c00 $^ --oformat binary

# '--oformat binary' deletes all symbols as a collateral, so we don't need
# to 'strip' them manually on this case
kernel.bin: boot/kernel_entry.o ${OBJ}
	$(LD) -o $@ -m elf_i386 -e main -Ttext 0x1000 $^ --oformat binary

# kernel.elf: boot/kernel_entry.o ${OBJ}
# 	$(LD) -o $@ -Ttext 0x1000 $^ 

run: os-image.bin
	qemu-system-x86_64 -fda os-image.bin

# Generic rules for wildcards
# To make an object, always compile from its .c
%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -ffreestanding -c $< -o $@

%.o: %.s
	${CC} $< -m32 -c -o $@

clean:
	rm -rf *.bin *.dis *.o os-image.bin *.elf
	rm -rf kernel/*.o boot/*.bin drivers/*.o boot/*.o cpu/*.o libc/*.o
