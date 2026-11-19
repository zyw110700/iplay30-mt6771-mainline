#!/bin/bash
# Package a boot image for the iPlay30 from the current kernel build.
#
#   ./package_boot.sh my_boot_stage1_i2c.img [extra-dtb]
#
# Uses the stage-1 initramfs (mounts /dev/mmcblk0p40 and switches root).
#
# IMPORTANT: the DTB *is* embedded and *is* what the kernel boots with (the
# header-v2 dtb section is honoured; the dtb/dtbo partitions are not used).
# The default is therefore mt6771-iplay30-hs400.dts compiled here, which is
# the patched device tree (mt6771-mmsys, mt6358-vproc/-vldo28, wifi@180f0000,
# no SPM PSCI idle states -> plain WFI).  Packaging vendor_no_idle.dtb by
# mistake is what made v23 look like a kernel regression: no DRM, no GPU
# clock, upstream MT6358 MFD stealing the PMIC and a reboot loop.
set -e
cd "$(dirname "$0")"

OUT=${1:?usage: package_boot.sh <output.img>}
DTB=${2:-iplay30.dtb}
if [ ! -f "$DTB" ] || [ "$DTB" = iplay30.dtb ]; then
    DTB=iplay30.dtb
    dtc -I dts -O dtb -o "$DTB" mt6771-iplay30-hs400.dts 2>/dev/null
fi
KERNEL=arch/arm64/boot/Image.gz
RAMDISK=../rootfs/stage1-initramfs.cpio.gz
CMDLINE="${CMDLINE:-console=tty0 loglevel=4 vmalloc=400M nokaslr efi=noruntime bootopt=64S3,32N2,64N2 fw_devlink=permissive}"

mkbootimg \
    --kernel "$KERNEL" \
    --ramdisk "$RAMDISK" \
    --dtb "$DTB" \
    --cmdline "$CMDLINE" \
    --base 0x40000000 --kernel_offset 0x0 \
    --ramdisk_offset 0x15000000 --tags_offset 0x14000000 \
    --dtb_offset 0x14000000 --pagesize 2048 \
    --header_version 2 --os_version 10.0.0 --os_patch_level 2020-09 \
    --output "$OUT"

sha256sum "$OUT"
ls -l "$OUT"
