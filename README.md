Reference link: [os-tutorial][1]
                [ucore][2]

[1]: https://github.com/cfenollosa/os-tutorial
[2]: https://github.com/kiukotsu/ucore

```
mkdir build
cd build
cmake ../
make
qemu-system-x86_64 -hda os-image.bin
```

```
# debug
qemu-system-i386 -s -S -hda os-image.bin -d guest_errors,int

gdb -ex "target remote localhost:1234"
```
