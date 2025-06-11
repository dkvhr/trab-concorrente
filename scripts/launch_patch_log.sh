KERNEL_VERSION=5.16.5
BUSYBOX_VERSION=1.36.1

echo "[+] Copying everything from the src folder to the system home..."
cp src/* busybox-$BUSYBOX_VERSION/initramfs/home/user
echo "[+] Generating initramfs..."
cd busybox-$BUSYBOX_VERSION/initramfs
find . -print0 | cpio --null -ov --format=newc --owner=+0:+0 > ../initramfs.cpio
gzip ./../initramfs.cpio
cd ../../

echo "[+] Running QEMU..."
echo "[+] Connect GDB to it to start running..."
qemu-system-x86_64 \
    -m 512M \
    -nographic \
    -kernel linux-$KERNEL_VERSION-patch-logico/arch/x86_64/boot/bzImage \
    -append "console=ttyS0 loglevel=3 oops=panic panic=-1 nopti nokaslr" \
    -no-reboot \
    -cpu qemu64 \
    -smp 4 \
    -monitor /dev/null \
    -initrd busybox-$BUSYBOX_VERSION/initramfs.cpio.gz \
    -fsdev local,security_model=passthrough,id=fsdev0,path=$HOME \
    -device virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=hostshare \
    -net nic,model=virtio \
    -net user \
    -gdb tcp::1234 \
    -S
