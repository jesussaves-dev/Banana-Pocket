---
name: banana-trust-boundaries
description: Use for defensive BananaOS review of which IPC peer may issue a command, untrusted input/memory safety, signed updates, archive handling, or LAN exposure. Not for routine session sequencing.
---

# BananaOS trust boundaries

1. **Trace the boundary.** Identify the untrusted input, caller, authority, checks and privileged effect. Cite file/line evidence and separate confirmed gaps from assumptions. In [native control](../../../system/banana-retro/src/control.c), check peer credentials and per-command role/session/version checks before effects. Reject truncated or invalid packets; check lengths, offsets, arithmetic, lifetimes and FD ownership before access or transfer. Preserve independent pause ownership and core safe points.
2. **Treat archives as untrusted.** Trace [update](../../../system/rootfs/usr/libexec/banana/update) and [backup restore](../../../system/rootfs/usr/libexec/banana/backup) from intake to extraction. Review normalized destination containment, member types/links, duplicates, size limits and cleanup on error. For updates verify release key and signed manifest coverage, board match, and payload hashes. Never weaken signature policy. Preserve inactive-slot writes and read-back before switching `extlinux.conf`, the known-good slot, and shared `/data`.
3. **Check exposure.** Read [boot service gating](../../../system/rootfs/etc/init.d/rcS) and the [web handler](../../../system/rootfs/usr/libexec/banana/banana-web.py). Keep web, SMB and SSH off by default; keep SSH key-only. Review PIN/session and request origin checks, upload bounds, and secret exclusion for listings and exports. Do not treat LAN access as authorization.
4. **Verify defensively.** Prefer static review and, when authorized, focused local fixtures and ASan/UBSan. No exploit development, live probing or secret disclosure. Follow the [engineering rules](../../../docs/ENGINEERING-RULES.md): hardware, deploy/flash, partition/bootloader and suspend/power need separate approval; QEMU only if explicitly required. Kernel/upstream patches need evidence and explicit approval. Host checks do not prove card durability.
