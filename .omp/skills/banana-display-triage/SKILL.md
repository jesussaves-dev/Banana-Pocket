---
name: banana-display-triage
description: Use for BananaOS banana-ui scanout, Wayland buffers, DMA-BUF, atomic KMS, frame timing, or black frames after a successful wake. Not for wake-source or native session policy.
---

# BananaOS display triage

1. **Scope.** Read the [display architecture](../../../docs/ARCHITECTURE.md#3-ui-engine-banana-ui) and [engineering rules](../../../docs/ENGINEERING-RULES.md#display-and-ui). Capture the client/backend, symptom, and first failing boundary. Preserve banana-ui DRM master and plane ownership; no secondary compositor or BFI.
2. **Trace the path.** In [compositor.cpp](../../../ui/src/compositor.cpp), check xdg configure/ack and the attached buffer. Distinguish SHM upload, EGL-transformed DMA-BUF, and direct scanout. Follow format/modifier, stride/offset, and FD ownership through [EGL/GBM](../../../ui/src/egl_presenter.cpp), [direct import](../../../ui/src/presenter.cpp), and [atomic KMS](../../../ui/src/kms.cpp). Preserve EGL-before-PRIME/AddFB2 for scanout slots. Check connector, CRTC, plane state, geometry, and commit errno before blaming the driver.
3. **Sync and pacing.** Follow native fence export to `IN_FENCE_FD` or the `glFinish()` fallback. Trace flip completion and `wl_buffer.release` before reuse; a frame callback is not proof of presentation. Use captured `3f-trace` and client timing first. Separate client cadence, render/fence wait, commit, and flip; compare p95/p99 flip intervals and missed vblanks at matched content, mode, backend, clocks, and thermals. [Host benchmark limits](../../../docs/PERFORMANCE.md).
4. **Recovery.** Trace drop, disconnect, and wake through `Display::restore` and `Compositor::resume`. Verify same-FD modeset, pending flip and buffer cleanup, and menu restoration. Do not close or steal DRM master to diagnose a stall.
5. **Evidence and gates.** Inspect the [video fixture](../../../system/banana-retro/Makefile) for protocol changes and [transpose test](../../../ui/tests/turn-test.cpp) for rotation. Run only authorized checks; report command, status, and limits. Host fixtures or QEMU boot do not qualify RPC scanout. QEMU needs explicit authorization; hardware probes, trace toggles on device, suspend/power, and deployment need separate approval. Kernel/upstream patches need evidence and explicit authorization.
