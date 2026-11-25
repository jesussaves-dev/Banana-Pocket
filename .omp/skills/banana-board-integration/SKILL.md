---
name: banana-board-integration
description: Use for Retroid Pocket Classic Buildroot cross-builds, stale packaged binaries including banana-retro, host/target staging, or kernel/DT integration. Not for runtime faults.
---

# BananaOS board integration

1. **Select evidence.** Use [AGENTS.md](../../../AGENTS.md) and the [engineering rules](../../../docs/ENGINEERING-RULES.md). For kernel/DT work, start with [boot architecture](../../../docs/ARCHITECTURE.md#2-boot--init), [defconfig](../../../buildroot/configs/banana_retroid_pocket_classic_defconfig) and [Rev Q DTS](../../../buildroot/board/retroid-pocket-classic/dts/qcom/sm4450-retroid-pocket-classic-revq.dts). Check the pin, config, bindings, firmware and probe order. Show why userspace or configuration is insufficient before proposing a kernel patch.
2. **Trace packages across the host/target boundary.** Follow [Kconfig](../../../buildroot/Config.in) and the package recipe from dependencies through staging and target install. The [banana-retro recipe](../../../buildroot/package/banana-retro/banana-retro.mk) shows host generators, target compiler and pkg-config/sysroot, and license inputs. Keep host binaries and native headers/libraries out of target artifacts; verify target ABI and dependency provenance.
3. **Locate stale artifacts.** The [top-level Makefile](../../../Makefile) owns local-package freshness. Check shared-header prerequisites, stamps, staging, target binary and final image. See the [board README](../../../buildroot/board/retroid-pocket-classic/README.md) for patch invalidation and overlay leftovers. A package rebuild is not an image rebuild. Preserve legal-source handling.
4. **Respect scope.** Read-only means inspect, not rebuild or flash. For authorized work, inspect commands first, prefer scoped checks, and report status and limits. Preserve boot order and A/B rollback. Kernel/upstream patches need evidence and explicit approval. QEMU must be explicitly required; hardware, deploy/flash, partitions/bootloaders and suspend/power need separate approval. Host/QEMU checks do not qualify RPC hardware.
