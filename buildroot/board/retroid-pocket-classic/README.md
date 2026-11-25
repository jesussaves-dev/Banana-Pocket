# Retroid Pocket Classic (SM4450 / Rev Q) — Banana Pocket board

This board reuses **ROCKNIX's SM4450 hardware layer** (kernel patches, Rev Q DTB,
ADSP/WPSS/ACF/QCA firmware) and **OpenABL** (already flashed) instead of U-Boot.
Banana's userspace, extlinux A/B boot and exFAT data partition stay.

Boot flow:

```
OpenABL (abl_a/abl_b)
  └─ FAT p1 BANANA-BOOT / extlinux/extlinux.conf
       ├─ slot a: /a/Image + /a/sm4450-retroid-pocket-classic-revq.dtb
       └─ slot b: /b/Image + /b/... (after an update)
            ↓
       squashfs p2 banana-root-a (or p3 -b), exFAT p4 BANANA
```

Partitions: p1 boot (FAT), p2 root A, p3 root B, p4 data (created on first boot).

The [board defconfig](../../configs/banana_retroid_pocket_classic_defconfig) selects
Linux 7.3 with `linux-banana.config`, the Banana and QEMU config fragments, and
`linux-patches/`. [KERNEL_SOURCE](KERNEL_SOURCE) records the patch stack's base
and tag. `linux-rocknix.config` is kept for comparison only.

Firmware in `firmware/` is installed to `/lib/firmware/` by `post-build.sh`.

The Wi-Fi is the **WCN3950** on `ath10k_snoc` (the DT's `qcom,wcn3990-wifi`): the blobs it asks for are
`ath10k/WCN3990/hw1.0/{firmware-5.bin,board-2.bin,wlanmdsp.mbn}`, and the WPSS remoteproc firmware is
`qcom/sm4450/RPClassic/wpss.mbn`. `rcS` loads the driver (`modprobe ath10k_snoc`); the regulatory database is the
`wireless-regdb` package. (No RTL8821CS: that was the RG DS.)

**Changing a package patch** under `buildroot/patches/<pkg>/` does not invalidate
the package's `.stamp_patched`. `<pkg>-rebuild` clears build/install stamps but
keeps the already-patched source; it does not apply a new patch or undo an old one.
After approval to discard that package's build directory and rebuild, run the
package's `-dirclean` target (for example `make retroarch-dirclean-retroid-pocket-classic`).
Then `make retroid-pocket-classic` extracts, patches, rebuilds and assembles the
image. Verify the patch step, installed target binary, and image before deployment.

**Patches to Buildroot itself** in `buildroot/buildroot-patches/` are handled by
the [top-level Makefile](../../../Makefile)'s `buildroot-patched` target, not
package `-dirclean`. The recipe skips patches already applied; it does not undo
removed or replaced patches. For those changes, reconcile the Buildroot checkout
against the pinned base before reapplying, preserving local edits and obtaining
approval before any destructive cleanup.

**Removing a file** from the rootfs overlay (`system/rootfs/...`) does not remove it from `output/<board>/target`:
Buildroot copies the overlay over `target/` without pruning, so a removed file stays and is baked into the next image
(a removed theme, a deleted binary -- anything). After removing one, delete it from `target/` as well, or wipe
`target/` for a clean slate, and rebuild.
