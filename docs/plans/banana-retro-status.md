# Banana-retro status (shelved)

- **Experimental.** The native runtime is not enabled by default; `runtime.conf` stays absent on devices.
- **Host verification only.** Persistence fixes (core SRAM/state sizes treated as capacities; oversized saves fail closed) pass `check-saves`, `check-control`, `check` and fixture/pinned-mGBA host runs (x86). Run fixture builds with `ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=0`.
- **Pending hardware qualification:** persistence relaunch and checkpoint resume on Retroid Pocket Classic with the aarch64 mGBA core; suspend/wake and power behavior.
- **Known performance issues:** native runtime performance is unresolved and was investigated separately; do not promote until qualified.
