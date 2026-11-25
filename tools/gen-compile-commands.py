#!/usr/bin/env python3
"""gen-compile-commands: a compile_commands.json for clangd over banana-ui, bananad and banana-retro.

The commands come from the packages' own Makefiles (`make -n -B`), given the variables Buildroot's .mk files give
them, so the database follows Makefile changes without a copy of their flags here. What differs from a Buildroot build:
  - the sources are this working tree's (Buildroot builds rsynced copies that go stale between image builds);
  - the shared IPC header is system/bananad/include (the staged copy under the sysroot is as old as the last bananad
    build), because the Makefiles' own -I points there unless a package build overrides it;
  - the Wayland protocol headers are generated here, from the Makefiles' own wayland-scanner commands, into
    .cache/banana-gen with Buildroot's host wayland-scanner;
  - the compiler is Clang aimed at Buildroot's toolchain: --target, --sysroot and --gcc-toolchain stand in for the
    GCC wrapper (whose -mcpu is not a Clang value), so clangd needs no --query-driver.
Everything else is the build's own: -std, -W*, -D*, -I and -pthread from the Makefiles; -D_LARGEFILE*, -D_FILE_OFFSET_BITS,
-O2, -g0 and -D_FORTIFY_SOURCE from Buildroot's TARGET_CFLAGS, which this script derives from <out>/<board>/.config
(Toolchain._target_cflags mirrors package/Makefile.in) and warns about when banana-ui's recorded flags differ.
-Werror and -Wshadow=local are removed in .clangd, not here: the database stays the real build's command.

Needs a configured Buildroot output (make retroid-pocket-classic once): <out>/<board>/.config, host/bin/*-linux-gcc,
host/bin/pkg-config, host/bin/wayland-scanner and the sysroot. Python 3 standard library only.

make's dry run (-n) still runs recipe lines that name $(MAKE), and every $(shell ...) at parse time. None of the goals
used here (banana-ui, banana-scrape, banana-store, bananad's all, banana-retro's all) reaches a $(MAKE) line: ui's
`host` target, banana-retro's server-header rule (CHECK_SERVER_HEADERS) and its check-* targets have one, but no goal
here depends on them. The $(shell ...) calls only query pkg-config (the scanner and protocol paths are overridden).
MAKE is also set to /bin/false so that a recursive recipe added to a reachable target fails this script instead of
running.

Publication: the commands, the file list and the coverage are validated, the protocol code is generated into a temp
dir and the database is written to a temp file, all before anything published is touched. Then the changed headers
are replaced, the database is replaced and stale headers are removed. A failure before that point leaves the previous
database and headers exactly as they were.
"""
import argparse
import glob
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_EXTS = (".c", ".cc", ".cpp", ".cxx")
# link-only arguments: harmless to Clang's front end but meaningless for a single translation unit
LINK_PREFIXES = ("-l", "-L", "-Wl,")
DEP_FLAGS_NOARG = ("-MMD", "-MP", "-MD", "-M", "-MM")
DEP_FLAGS_ARG = ("-MF", "-MT", "-MQ")


def die(msg):
    sys.exit("gen-compile-commands: error: " + msg)


def shown(path):
    """A path for messages: repo-relative inside the repo, as given elsewhere."""
    rel = os.path.relpath(path, REPO)
    return path if rel.startswith("..") else rel


def need(path, what, hint="run `make retroid-pocket-classic` once to populate the Buildroot output"):
    if not os.path.exists(path):
        die("missing %s: %s\n  (%s)" % (what, path, hint))
    return path


def run(argv, env=None, cwd=None):
    try:
        p = subprocess.run(argv, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    except OSError as e:
        die("cannot run %s: %s" % (argv[0], e))
    return p


class Toolchain:
    def __init__(self, out_dir, board):
        self.root = os.path.abspath(os.path.join(out_dir, board))
        if not os.path.isdir(self.root):
            die("no Buildroot output for board %r at %s\n  (run `make %s` once)" % (board, self.root, board))
        self.host_bin = need(os.path.join(self.root, "host", "bin"), "Buildroot host/bin")
        self.config = self._read_config(need(os.path.join(self.root, ".config"), "Buildroot .config"))

        gccs = glob.glob(os.path.join(self.host_bin, "*-linux-gcc"))
        if len(gccs) != 1:
            die("expected exactly one host/bin/*-linux-gcc in %s, found %d" % (self.host_bin, len(gccs)))
        self.cc = gccs[0]
        self.cxx = need(self.cc[:-len("gcc")] + "g++", "the toolchain's g++")
        self.pkg_config = need(os.path.join(self.host_bin, "pkg-config"), "host pkg-config wrapper")
        self.scanner = need(os.path.join(self.host_bin, "wayland-scanner"), "host wayland-scanner",
                            "Buildroot's host-wayland package builds it")

        def query(opt):
            p = run([self.cc, opt])
            if p.returncode != 0 or not p.stdout.strip():
                die("%s %s failed: %s" % (self.cc, opt, p.stderr.strip() or "no output"))
            return p.stdout.strip()

        self.triple = query("-dumpmachine")
        self.sysroot = os.path.realpath(query("-print-sysroot"))
        if not os.path.isdir(os.path.join(self.sysroot, "usr", "include")):
            die("sysroot has no usr/include: %s" % self.sysroot)
        libgcc = os.path.realpath(query("-print-libgcc-file-name"))
        # <root>/lib/gcc/<triple>/<version>/libgcc.a
        self.gcc_toolchain = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(libgcc)))))
        need(os.path.join(self.gcc_toolchain, "lib", "gcc", self.triple), "the toolchain's lib/gcc/<triple> (libgcc.a: %s)" % libgcc)
        self.protocols = need(os.path.join(self.sysroot, "usr", "share", "wayland-protocols"),
                              "staged wayland-protocols (xdg-shell, linux-dmabuf, viewporter XML)")

        self.clang = shutil.which("clang")
        self.clangxx = shutil.which("clang++")
        if not self.clang or not self.clangxx:
            die("clang and clang++ must be on PATH (they provide clangd's compiler front end)")

        self.target_cflags = self._target_cflags()

    @staticmethod
    def _read_config(path):
        cfg = {}
        with open(path) as f:
            for line in f:
                line = line.rstrip("\n")
                if line.startswith("BR2_") and "=" in line:
                    k, v = line.split("=", 1)
                    cfg[k] = v.strip('"')
        return cfg

    def _target_cflags(self):
        """Buildroot's TARGET_CFLAGS (package/Makefile.in): the same string TARGET_CXXFLAGS holds."""
        c = self.config
        flags = ["-D_LARGEFILE_SOURCE", "-D_LARGEFILE64_SOURCE", "-D_FILE_OFFSET_BITS=64"]
        if c.get("BR2_TIME_BITS_64") == "y":
            flags.append("-D_TIME_BITS=64")
        opt = None
        for key, val in (("BR2_OPTIMIZE_0", "-O0"), ("BR2_OPTIMIZE_1", "-O1"), ("BR2_OPTIMIZE_2", "-O2"),
                         ("BR2_OPTIMIZE_3", "-O3"), ("BR2_OPTIMIZE_G", "-Og"), ("BR2_OPTIMIZE_S", "-Os"),
                         ("BR2_OPTIMIZE_FAST", "-Ofast")):
            if c.get(key) == "y":
                opt = val
        if opt:
            flags.append(opt)
        flags += shlex.split(c.get("BR2_TARGET_OPTIMIZATION", ""))
        debug = "-g0" if "BR2_ENABLE_DEBUG" not in c else None
        for key, val in (("BR2_DEBUG_1", "-g1"), ("BR2_DEBUG_2", "-g2"), ("BR2_DEBUG_3", "-g3")):
            if c.get(key) == "y":
                debug = val
        if debug:
            flags.append(debug)
        for n in ("1", "2", "3"):
            if c.get("BR2_FORTIFY_SOURCE_" + n) == "y":
                flags.append("-D_FORTIFY_SOURCE=" + n)
                break
        return flags

    def env(self):
        # a minimal environment: CPPFLAGS/LDFLAGS/PKG_CONFIG_* of the caller must not leak into the commands
        return {"PATH": self.host_bin + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C",
                "HOME": os.environ.get("HOME", "/")}


def dry_run(tc, subdir, variables, goals):
    """`make -n -B` in REPO/subdir; returns its logical command lines (backslash continuations joined)."""
    d = os.path.join(REPO, subdir)
    argv = ["make", "-n", "-B", "--no-print-directory", "-C", d, "MAKE=/bin/false"]
    argv += ["%s=%s" % kv for kv in variables]
    argv += goals
    p = run(argv, env=tc.env())
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        die("`make -n -B` failed in %s (exit %d)" % (subdir, p.returncode))
    lines, cur = [], ""
    for raw in p.stdout.split("\n"):
        if raw.endswith("\\"):
            cur += raw[:-1] + " "
            continue
        cur += raw
        if cur.strip():
            lines.append(cur)
        cur = ""
    return d, lines


def split_command(line):
    try:
        return shlex.split(line)
    except ValueError:
        return None  # a shell construct this parser does not model; not a compiler or scanner line


def compile_entries(tc, subdir, compiler, lines, directory):
    """One entry per source file in each of `compiler`'s lines; scanner lines are returned for generation."""
    entries, scanner_cmds = [], []
    for line in lines:
        argv = split_command(line)
        if not argv:
            continue
        if argv[0] == tc.scanner:
            if len(argv) != 4:
                die("unexpected wayland-scanner command in %s: %s" % (subdir, line))
            scanner_cmds.append([argv[1], os.path.normpath(os.path.join(directory, argv[2])), argv[3]])
            continue
        if argv[0] != compiler:
            continue
        flags, sources = [], []
        i, force_c = 1, False
        while i < len(argv):
            a = argv[i]
            if a == "-o":
                i += 2
                continue
            if a in DEP_FLAGS_ARG:
                i += 2
                continue
            if a in DEP_FLAGS_NOARG or a in ("-c", "-shared") or a.startswith(LINK_PREFIXES):
                i += 1
                continue
            if a == "-x" and i + 1 < len(argv):
                force_c = force_c or argv[i + 1] == "c"
                flags += [a, argv[i + 1]]
                i += 2
                continue
            if not a.startswith("-") and a.endswith(SOURCE_EXTS):
                sources.append(a)
            elif not a.startswith("-") and a.endswith((".o", ".a", ".so")):
                pass  # a link input of the linking line, not a flag
            else:
                flags.append(a)
            i += 1
        for src in sources:
            path = os.path.normpath(os.path.join(directory, src))
            is_c = force_c or path.endswith(".c")
            driver = tc.clang if is_c or not compiler.endswith("g++") else tc.clangxx
            entries.append({
                "directory": directory,
                "arguments": [driver, "--target=" + tc.triple, "--sysroot=" + tc.sysroot,
                              "--gcc-toolchain=" + tc.gcc_toolchain] + flags + ["-c", path],
                "file": path,
            })
    return entries, scanner_cmds


def plan_generated(gen_dir, scanner_cmds):
    """Validate the Makefiles' wayland-scanner commands without writing anything: {output path: (mode, xml)}."""
    wanted = {}
    for mode, xml, out in scanner_cmds:
        out = os.path.abspath(out)
        if os.path.dirname(out) != gen_dir:
            die("wayland-scanner output outside %s: %s" % (gen_dir, out))
        need(xml, "protocol XML %s" % xml, "a package Makefile's protocol source")
        wanted[out] = (mode, xml)
    return wanted


def stage_generated(wanted, tc, tmp):
    """Run every scanner command into tmp; nothing published is touched. Returns [(staged, final, changed)]."""
    staged_files = []
    for out, (mode, xml) in sorted(wanted.items()):
        staged = os.path.join(tmp, os.path.basename(out))
        p = run([tc.scanner, mode, xml, staged])
        if p.returncode != 0 or not os.path.exists(staged):
            die("wayland-scanner %s %s failed: %s" % (mode, xml, p.stderr.strip()))
        with open(staged, "rb") as f:
            new = f.read()
        old = None
        if os.path.exists(out):
            with open(out, "rb") as f:
                old = f.read()
        staged_files.append((staged, out, old != new))
    return staged_files


def publish_generated(gen_dir, staged_files):
    """Replace the changed headers (same filesystem as the temp dir: atomic per file)."""
    os.makedirs(gen_dir, exist_ok=True)
    for staged, out, changed in staged_files:
        if changed:
            os.replace(staged, out)


def prune_generated(gen_dir, wanted):
    for stale in glob.glob(os.path.join(gen_dir, "*-protocol.[ch]")):
        if stale not in wanted:
            os.remove(stale)


def expected_units(gen_files):
    """Production translation units that must be covered, found on disk and not from the dry runs."""
    units = set(glob.glob(os.path.join(REPO, "ui", "src", "*.cpp")))
    units.add(os.path.join(REPO, "ui", "third_party", "qrcodegen.c"))  # linked into banana-ui (src/ has no .c)
    units.add(os.path.join(REPO, "system", "bananad", "bananad.c"))
    units.update(glob.glob(os.path.join(REPO, "system", "banana-retro", "src", "*.c")))
    units.update(f for f in gen_files if f.endswith(".c"))
    return units


def stage_db(path, text):
    """Write the database to a temp file beside `path` (same filesystem, so os.replace is atomic); returns its name."""
    d = os.path.dirname(os.path.abspath(path))
    try:
        fd, tmp = tempfile.mkstemp(prefix=os.path.basename(path) + ".", dir=d)
    except OSError as e:
        die("cannot create a temporary file for %s in %s: %s\n  (the previous database, if any, is unchanged; pass "
            "--output <file> in a writable directory)" % (os.path.basename(path), d, e.strerror or e))
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
    except OSError as e:
        os.remove(tmp)
        die("cannot write %s: %s (the previous database, if any, is unchanged)" % (tmp, e.strerror or e))
    return tmp


def check_flag_drift(tc, cxx_flags):
    """banana-ui's last Buildroot build recorded its exact command line; a mismatch means TARGET_CFLAGS drifted."""
    rec = os.path.join(tc.root, "build", "banana-ui-local", "build", "obj", ".flags")
    if not os.path.exists(rec):
        return
    with open(rec) as f:
        got = shlex.split(f.read())
    want = [tc.cxx] + cxx_flags + ["-std=c++17"]
    if got[:len(want)] != want:
        sys.stderr.write("gen-compile-commands: warning: Buildroot's recorded banana-ui flags differ from the derived "
                         "TARGET_CXXFLAGS; update Toolchain._target_cflags\n  recorded: %s\n  derived:  %s\n"
                         % (" ".join(got[:len(want)]), " ".join(want)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out-dir", default=os.path.join(REPO, "output"), help="Buildroot output root (default: output)")
    ap.add_argument("--board", default="retroid-pocket-classic")
    ap.add_argument("--output", default=os.path.join(REPO, "compile_commands.json"))
    ap.add_argument("--gen-dir", default=os.path.join(REPO, ".cache", "banana-gen"),
                    help="where the generated Wayland protocol code goes (default: .cache/banana-gen)")
    args = ap.parse_args()

    tc = Toolchain(args.out_dir, args.board)
    gen_dir = os.path.abspath(args.gen_dir)
    cflags = tc.target_cflags

    # the .mk files' variables: banana-ui CXXFLAGS="$(TARGET_CXXFLAGS) -O2", bananad CFLAGS="$(TARGET_CFLAGS)",
    # banana-retro CFLAGS="$(TARGET_CFLAGS) -O2"; LDFLAGS empty (TARGET_LDFLAGS only matters when linking)
    ui_cxxflags = cflags + ["-O2"]
    check_flag_drift(tc, ui_cxxflags)
    # banana-ui's other Buildroot overrides are left to the Makefile's defaults on purpose: BANANA_CONTROL_INCLUDE
    # (the repo's system/bananad/include, not the staged copy) and ALSA=1
    ui_dir, ui_lines = dry_run(tc, "ui", [
        ("CXX", tc.cxx), ("PKG_CONFIG", tc.pkg_config), ("CXXFLAGS", " ".join(ui_cxxflags)), ("LDFLAGS", ""),
        ("WAYLAND_SCANNER", tc.scanner), ("WAYLAND_PROTOCOLS", tc.protocols), ("WLDIR", gen_dir),
        ("BANANA_RETRO_XML", os.path.join(REPO, "system", "banana-retro", "protocol", "banana-retro-v1.xml")),
    ], ["banana-ui", "banana-scrape", "banana-store"])
    bd_dir, bd_lines = dry_run(tc, os.path.join("system", "bananad"), [
        ("CC", tc.cc), ("CFLAGS", " ".join(cflags)), ("LDFLAGS", ""),
    ], ["all"])
    br_dir, br_lines = dry_run(tc, os.path.join("system", "banana-retro"), [
        ("CC", tc.cc), ("PKG_CONFIG", tc.pkg_config), ("CFLAGS", " ".join(cflags + ["-O2"])), ("LDFLAGS", ""),
        ("WAYLAND_SCANNER", tc.scanner), ("WAYLAND_PROTOCOLS", tc.protocols), ("OUT", gen_dir),
    ], ["all"])

    entries, scanner_cmds, counts = [], [], {}
    for name, subdir, compiler, lines, directory in (
            ("banana-ui", "ui", tc.cxx, ui_lines, ui_dir),
            ("bananad", "system/bananad", tc.cc, bd_lines, bd_dir),
            ("banana-retro", "system/banana-retro", tc.cc, br_lines, br_dir)):
        e, s = compile_entries(tc, subdir, compiler, lines, directory)
        counts[name] = len(e)
        entries += e
        scanner_cmds += s
    if not entries:
        die("the dry runs produced no compile commands")

    # ui's Makefile also compiles the generated protocol .c with CXX as C (-x c); banana-retro lists the same files
    # as plain CC lines. The first command for a file wins (banana-ui, bananad, banana-retro order).
    seen, unique = set(), []
    for e in entries:
        if e["file"] not in seen:
            seen.add(e["file"])
            unique.append(e)

    scanner_cmds = [list(c) for c in sorted({tuple(c) for c in scanner_cmds})]
    wanted = plan_generated(gen_dir, scanner_cmds)
    gen_files = sorted(wanted)

    # generated files are named by the scanner commands; they do not exist yet on a fresh tree
    missing_files = sorted(e["file"] for e in unique if e["file"] not in wanted and not os.path.exists(e["file"]))
    if missing_files:
        die("compile commands name files that do not exist:\n  " + "\n  ".join(missing_files))
    uncovered = sorted(expected_units(gen_files) - {e["file"] for e in unique})
    if uncovered:
        die("production translation units without a compile command (a Makefile no longer builds them, or the "
            "dry-run parser missed them):\n  " + "\n  ".join(os.path.relpath(u, REPO) for u in uncovered))
    unique.sort(key=lambda e: e["file"])

    # stage everything, then publish: a failure up to here leaves the previous database and headers untouched
    db_tmp = None
    tmp_dir = os.path.dirname(gen_dir)
    try:
        try:
            os.makedirs(tmp_dir, exist_ok=True)
            stage = tempfile.TemporaryDirectory(prefix=".gen.", dir=tmp_dir)
        except OSError as e:
            die("cannot create a staging directory in %s: %s\n  (pass --gen-dir <dir> in a writable location)"
                % (tmp_dir, e.strerror or e))
        with stage as tmp:
            try:
                staged_files = stage_generated(wanted, tc, tmp)
            except OSError as e:
                die("cannot stage the generated protocol code in %s: %s (nothing published was changed)"
                    % (tmp, e.strerror or e))
            db_tmp = stage_db(args.output, json.dumps(unique, indent=2) + "\n")
            try:
                publish_generated(gen_dir, staged_files)
                os.replace(db_tmp, args.output)
                prune_generated(gen_dir, wanted)
            except OSError as e:
                die("cannot publish into %s or %s: %s\n  (fix the path or its permissions and rerun; make compdb is "
                    "safe to repeat)" % (gen_dir, args.output, e.strerror or e))
    finally:
        if db_tmp and os.path.exists(db_tmp):
            os.remove(db_tmp)
    print("wrote %s: %d entries (%s); %d generated protocol files in %s"
          % (shown(args.output), len(unique),
             ", ".join("%s %d" % kv for kv in counts.items()), len(gen_files), shown(gen_dir)))


if __name__ == "__main__":
    main()
