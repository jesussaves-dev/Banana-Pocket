#!/usr/bin/env python3
"""One-time K3 import: turn linux-patches/*.patch into commits on a v7.2 tree.

Usage: tools/kernel-repo-import.py <banana-linux worktree at v7.2>

Each patch is rewritten as a well-formed mbox and applied with `git am`:
  - From:    kept; missing -> Tom (all such patches arrived in Tom's
             2026-10-27 port checkpoint commit)
  - Date:    kept; missing -> the date the patch file was first added
             to this repo (git log --diff-filter=A)
  - Subject: kept (wherever it sits in the preamble); missing -> SUBJECTS
  - the rest of the preamble becomes the commit body
Committer is Tom with committer date == author date, so the commit hashes
are reproducible from the same inputs.

This ran once to create banana-7.2-1; afterwards banana-linux is the source
and linux-patches/ is generated from it (tools/kernel-patches-gen.sh).
"""
import os
import re
import subprocess
import sys
import tempfile

TOM = "Tom <jesussaves-ai@users.noreply.github.com>"
SUBJECTS = {
    "0011": "clk: qcom: dispcc-sm4450: use the no-init-park ops for the MDP clock source",
    "0018": "clk: qcom: gpucc-sm4450: retain the GX GDSC registers",
    "0030": "ASoC: qcom: sc8280xp: support the I2S clocks",
}
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATCHDIR = os.path.join(REPO, "buildroot/board/retroid-pocket-classic/linux-patches")
DIFF_START = re.compile(r"^(diff --git |--- a/|--- /dev/null)")


def git(*args, cwd=REPO, env=None):
    return subprocess.run(["git", *args], cwd=cwd, env=env, check=True,
                          capture_output=True, text=True).stdout


def added_date(path):
    out = git("log", "--diff-filter=A", "--follow", "--format=%aD", "--",
              os.path.relpath(path, REPO)).split("\n")
    out = [l for l in out if l]
    if not out:
        sys.exit("no add commit for " + path)
    return out[-1]


def normalize(path):
    name = os.path.basename(path)
    lines = open(path, encoding="utf-8").read().split("\n")
    start = next(i for i, l in enumerate(lines) if DIFF_START.match(l))
    pre, diff = lines[:start], lines[start:]
    # drop the format-patch signature ("-- " + version) at the end
    while diff and diff[-1] == "":
        diff.pop()
    if len(diff) >= 2 and diff[-2] == "-- ":
        diff = diff[:-2]
    hdr = {}
    body = []
    i = 0
    while i < len(pre):
        l = pre[i]
        m = re.match(r"^(From|Date|Subject): (.*)$", l)
        if l.startswith("From ") and re.match(r"^From [0-9a-f]{40} ", l):
            i += 1
            continue
        if m and m.group(1) not in hdr:
            val = m.group(2)
            while i + 1 < len(pre) and pre[i + 1][:1] in (" ", "\t") and pre[i + 1].strip():
                i += 1
                val += " " + pre[i].strip()
            hdr[m.group(1)] = val
        else:
            body.append(l)
        i += 1
    # the "---" separator and diffstat belong to the old mail, not the body
    if "---" in body:
        body = body[:body.index("---")]
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    subj = hdr.get("Subject") or "[PATCH] " + SUBJECTS[name[:4]]
    subj = re.sub(r"^\[PATCH[^\]]*\]\s*", "", subj)
    out = [
        "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001",
        "From: " + hdr.get("From", TOM),
        "Date: " + hdr.get("Date", added_date(path)),
        "Subject: [PATCH] " + subj,
        "",
    ]
    if body:
        out += body + [""]
    out += ["---"] + diff + [""]
    return "\n".join(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tree = sys.argv[1]
    env = dict(os.environ, GIT_COMMITTER_NAME="Tom",
               GIT_COMMITTER_EMAIL="jesussaves-ai@users.noreply.github.com")
    patches = sorted(f for f in os.listdir(PATCHDIR) if f.endswith(".patch"))
    with tempfile.TemporaryDirectory() as tmp:
        for f in patches:
            mbox = os.path.join(tmp, f)
            with open(mbox, "w", encoding="utf-8") as fh:
                fh.write(normalize(os.path.join(PATCHDIR, f)))
            r = subprocess.run(["git", "am", "-q", "--committer-date-is-author-date", mbox],
                               cwd=tree, env=env, capture_output=True, text=True)
            if r.returncode:
                subprocess.run(["git", "am", "--abort"], cwd=tree)
                sys.exit(f"git am failed on {f}:\n{r.stdout}{r.stderr}")
            print("ok", f)


if __name__ == "__main__":
    main()
