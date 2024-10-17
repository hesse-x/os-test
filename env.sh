sudo apt install qemu qemu-kvm libvirt-daemon-system libvirt-clients bridge-utils virt-manager nasm

# GCC also compile, no need to compile cross-compiler
# Compile i686-elf clang
# cmake -G Ninja \
#   -DCMAKE_BUILD_TYPE=Release \
#   -DLLVM_TARGETS_TO_BUILD="X86" \
#   -DLLVM_DEFAULT_TARGET_TRIPLE="i686-elf" \
#   -DLLVM_ENABLE_PROJECTS="clang;lld" \
#   -DLLVM_ENABLE_RUNTIMES="compiler-rt" \
#   -DCMAKE_INSTALL_PREFIX=/usr/local \
#   ../llvm
