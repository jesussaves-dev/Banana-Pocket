#!/usr/bin/env python3
"""check-docs: every repo path a doc, page or code comment names must exist.

Scans the tracked Markdown, site/*.html and the sources under ui/src, system, tools, app and buildroot/package for
  - Markdown links and HTML href/src that point inside the repo (relative, no scheme), and
  - repo paths written out in text: docs/..., tools/..., ui/..., system/..., buildroot/..., app/..., site/...
and checks them against `git ls-files` (a directory counts if a tracked file is under it). Globs and placeholders
(`*`, `<...>`, `{a,b}`, `...`) are skipped, as are paths inside `git show <rev>:` pointers (history, on purpose).
Prints one line per dangling reference and exits 1, or prints nothing and exits 0.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PREFIXES = ("docs/", "tools/", "ui/", "system/", "buildroot/", "app/", "site/")
SCAN_EXT = (".md", ".html", ".c", ".h", ".cpp", ".py", ".sh", ".mk", ".in", ".conf", ".json")
SCAN_DIRS = ("ui/src/", "system/", "tools/", "app/", "buildroot/package/", "site/", "docs/", ".github/")
SKIP = ("docs/reports/", "ui/tests/golden/")
# build outputs and local-only notes: named on purpose, never tracked
ALLOW = ("ui/banana-ui", "ui/tests/out", "tools/drm-plane-probe", "tools/route-probe", "tools/rpc-gpu-probe",
         "docs/superpowers/", "output/",
         "app/src/main/res/")  # JorreFog/helios's tree, credited in THIRD_PARTY.md

# a repo path in prose: one of the prefixes, not glued to a preceding path or word character
TEXT_PATH = re.compile(r"(?<![\w/.~$-])((?:%s)[\w./+-]*[\w/])" % "|".join(re.escape(p) for p in PREFIXES))
MD_LINK = re.compile(r"\]\(([^)\s]+)\)")
HTML_REF = re.compile(r"""(?:href|src)\s*=\s*["']([^"']+)["']""")
GIT_SHOW = re.compile(r"git show [0-9a-f]{6,40}:\S+")


def tracked():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True).stdout
    files = set(out.decode().split("\0")) - {""}
    dirs = set()
    for f in files:
        d = os.path.dirname(f)
        while d and d not in dirs:
            dirs.add(d)
            d = os.path.dirname(d)
    return files, dirs


def exists(path, files, dirs):
    path = os.path.normpath(path)
    return path in files or path in dirs


def placeholder(p):
    return any(c in p for c in "*<>{}$") or "..." in p or "…" in p


def main():
    files, dirs = tracked()
    bad = []
    for f in sorted(files):
        if not f.endswith(SCAN_EXT) or f.startswith(SKIP):
            continue
        if not (f.endswith(".md") or f.startswith(SCAN_DIRS)):
            continue
        try:
            text = open(os.path.join(ROOT, f), encoding="utf-8").read()
        except (OSError, UnicodeDecodeError):
            continue
        here = os.path.dirname(f)
        for n, line in enumerate(text.split("\n"), 1):
            line = GIT_SHOW.sub("", line)
            refs = []
            if f.endswith((".md", ".html")):
                for m in MD_LINK.findall(line) + HTML_REF.findall(line):
                    if re.match(r"^[a-z][a-z0-9+.-]*:|^#|^//|^/", m) or placeholder(m):
                        continue
                    refs.append(os.path.join(here, m.split("#")[0].split("?")[0]))
            for mo in TEXT_PATH.finditer(line):
                m = mo.group(1).rstrip(".")
                rest = re.match(r"[^\s`'\")\]|,;]*", line[mo.start():]).group(0)
                if placeholder(m) or placeholder(rest) or m.endswith((".o", ".tmp")):
                    continue
                refs.append(m)
            for r in refs:
                r = re.sub(r":\d+(-\d+)?$", "", r)
                if not r or r.startswith(ALLOW) or exists(r, files, dirs):
                    continue
                # a patch named by its number (buildroot/patches/retroarch/0013) or a file by its stem
                if any(t.startswith(os.path.normpath(r) + c) for t in files for c in "-. "):
                    continue
                bad.append("%s:%d: %s" % (f, n, os.path.normpath(r)))
    for b in bad:
        print(b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
