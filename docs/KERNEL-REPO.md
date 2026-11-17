# banana-linux: the kernel as a git branch (K3)

The kernel Banana ships is mainline Linux plus a stack of Banana patches. Since K3 (kernel 0.4) that stack lives as
commits in **banana-linux**, <https://github.com/jesussaves-dev/banana-linux>, a fork of torvalds/linux:

| ref | what |
|---|---|
| `v7.2` | mainline, as Linus tagged it |
| branch `banana-7.2` | `v7.2` + the Banana commits, each with its original author |
| tag `banana-7.2-N` | a released state of that branch; `N` goes up each time the patch dir is regenerated |

`buildroot/board/retroid-pocket-classic/linux-patches/` is **generated** from that tag, so Buildroot keeps applying a
patch dir on top of the 7.2 tarball as it always has (`BR2_LINUX_KERNEL_PATCH`). Which tag the dir comes from is
written down in `buildroot/board/retroid-pocket-classic/KERNEL_SOURCE`. Don't edit a patch in the dir: change the
commit in banana-linux and regenerate.

`git log --format='%an' v7.2..banana-7.2 | sort | uniq -c` shows whose work the stack is: what is ours (Tom, Banana,
Jorre) and what came from other ports (sunshineinabox, ROCKNIX, Batocera and others) and is still to be upstreamed or replaced.

## Changing a patch

```sh
git clone --depth 1 --branch banana-7.2 https://github.com/jesussaves-dev/banana-linux ~/banana-linux
cd ~/banana-linux && git fetch --depth 1 origin tag v7.2     # the base, for v7.2..
# edit, then either a new commit, or fix an old one:
git commit --fixup=<commit> && git rebase -i --autosquash v7.2
git tag -a banana-7.2-2 -m "banana-7.2-2: <what changed>"
git push origin banana-7.2 banana-7.2-2                       # (--force-with-lease after a rebase)

cd ~/banana-rpc
tools/kernel-patches-gen.sh ~/banana-linux v7.2 banana-7.2-2  # rewrites linux-patches/
# bump BANANA_LINUX_TAG in KERNEL_SOURCE, build (make retroid-pocket-classic), boot (tools/qemu-boot.sh), commit
```

CI (`kernel` job) checks that `linux-patches/` is exactly what `KERNEL_SOURCE`'s tag generates. If you forget to push
the tag or bump the file, the job fails.

## Why the output is byte-stable

`tools/kernel-patches-gen.sh` runs `git format-patch` with every option that could change its output pinned. It also
ignores the user and system git config:

| option | why |
|---|---|
| `--zero-commit` | `From 0000…`: a rebase that only moves a commit does not rewrite its patch |
| `--no-signature` | no `-- \n2.x.y` trailer, which would change with the git version |
| `-N` | `[PATCH]`, not `[PATCH 07/45]`: inserting a patch renames files but leaves the others' contents alone |
| `--full-index` | full blob ids; abbreviated ones grow with the object count, so shallow and full clones would differ |
| `--attr-source=<tip>` | the kernel's `.gitattributes` (`diff=cpp`, `diff=dts`: function names in hunk headers) read from the tip, so a bare or shallow clone gives the same output as a worktree (needs git ≥ 2.40) |
| `--no-renames --diff-algorithm=myers --encode-email-headers --filename-max-length=64` | git's defaults, spelled out so `diff.*` and `format.*` config cannot change them |

Checked on 2026-11-16: the same 45 files come out of a full clone, a `--depth 46` bare clone (what CI fetches), and a
v7.2 tarball tree that the patches were `git am`'d onto.

## The import (banana-7.2-1)

`tools/kernel-repo-import.py` turned the old hand-kept dir into commits, once. The old dir was not format-patch
output. Some files were bare diffs, some had no `From:` or `Date:`, and one had a blank line before its `Subject:`. So
the first generated dir differs from the old one in every file: new headers, diffstats and hunk context, and numbering
0001–0045 with no gaps. **The kernel source it produces is identical.** `patch -p1` (as Buildroot applies it) of the
old dir and of the new dir onto the 7.2 tarball give the same tree, `e50735681b31`. The cumulative `git diff v7.2`
matches the old dir's too. Authorship:

- `From:` kept where the patch had one.
- Nine patches had none: 0011, 0018, 0030 (bare diffs), 0046–0049, 0051, 0052. They all arrived in Tom's 2026-10-27
  port checkpoint and are attributed to Tom. The three bare diffs were given subjects.
- `Date:` kept where present. Otherwise it is the date the file was first committed to Banana-Pocket.
- Committer Tom, committer date = author date, so the import is reproducible down to the commit ids.

## Moving to a new mainline (rebase recipe)

```sh
cd ~/banana-linux
git fetch --depth 1 origin tag v7.3
git switch -c banana-7.3 banana-7.2
git rebase --onto v7.3 v7.2          # resolve, `git rebase --continue`; empty commits = upstreamed, drop them
git tag -a banana-7.3-1 -m "banana-7.3-1"
git push origin banana-7.3 banana-7.3-1
```

Then, in Banana-Pocket:
1. Set `BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION` to the 7.3 tarball.
2. Set `KERNEL_SOURCE` to `v7.3` / `banana-7.3-1`.
3. Run `tools/kernel-patches-gen.sh ~/banana-linux v7.3 banana-7.3-1`.
4. Refresh `linux-banana.config` (`make linux-update-defconfig` after `olddefconfig`) and check its diff.
5. Do the K0 kernel report, sleep/wake and sound-test runs on the device before shipping.

### Conflict notes: trial rebase onto v7.3 (2026-11-16)

Each commit was cherry-picked onto `v7.3` on its own. 39 applied cleanly.

| commit | at 7.3 | note |
|---|---|---|
| phy: qcom: qmp-combo: Correct pre-emphasis table for QMP v4 DP PHYs (Esteban Urrutia) | **empty** | upstream in 7.3: drop |
| phy: qcom: qmp-combo: Prevent unnecessary PM runtime suspend at boot (Loic Poulain) | **empty** | upstream in 7.3: drop |
| drm/msm: add SM4450 display support | conflict, `drivers/soc/qcom/ubwc_config.c`, 2 hunks | 7.3 reworked the UBWC table into shared `ubwc_X_Y_hbbN` entries keyed by compatible: add `qcom,sm4450` to the match list with the matching shared entry instead of a private `sm4450_data` |
| regulator: qcom-rpmh: add PBS trigger resource type | conflict, `qcom-rpmh-regulator.c`, 1 hunk | the register `#define`s next to `RPMH_REGULATOR_ENABLE_MASK` moved: keep both |
| ASoC: qcom: sc8280xp: support the I2S clocks | conflict, `sound/soc/qcom/sc8280xp.c`, 3 hunks | 7.3 moved the card into `qcom_snd_soc_common` (`priv`, `qcom_snd_sdw_startup/shutdown`, `data->priv->driver_name`): re-do the I2S clock enable as startup/shutdown wrappers around the common ones |
| phy: qcom: qmp-combo: add SM4450 USB3/DP configuration | conflict, `phy-qcom-qmp-combo.c`, 4 hunks | 7.3 added sm8475 tables and configs at the same place: keep both, then check `.regs` / `.calibrate_dp_phy` on the SM4450 cfg by hand |

The next rebase writes its own notes here. Keep a row until the patch is gone.

## CI

The `kernel` job in `.github/workflows/ci.yml` runs on every push and pull request on the `kernel` branch. Elsewhere it
runs when kernel inputs change (`linux-*`, `dts/`, `KERNEL_*`, the defconfigs, `tools/qemu-boot.sh`,
`tools/kernel-*`). It:

1. fetches `KERNEL_SOURCE`'s base and tag (bare, `--depth 1` and `--depth <patches + 1>`), and checks the dir with
   `tools/kernel-patches-gen.sh --check`;
2. builds the kernel and its modules with Buildroot (`make linux`; the external Bootlin toolchain, so no
   toolchain build);
3. downloads the latest release image, replaces slot a's Image and `/lib/modules` with this build
   (`tools/kernel-into-image.sh`), and runs the whole `tools/qemu-boot.sh` test on it.
