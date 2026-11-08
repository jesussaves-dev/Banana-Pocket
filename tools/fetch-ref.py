#!/usr/bin/env python3
"""Prepare a clean, isolated hardware reference for tools/gen-icon.py's --ref (img2img).

    python3 tools/fetch-ref.py --query "Sega Dreamcast controller" --name dreamcast
    python3 tools/fetch-ref.py --local ui/assets/overrides/icons/dreamcast.png --name dreamcast

Source A (--query) searches Wikimedia Commons (namespace 6, files) for product photos,
preferring images that already have a transparent or plain white studio background.
Source B (--local) takes an existing icon or any local image.

Either way the subject is cut out (rembg's small u2netp model when it is installed in
~/ai-tools/ComfyUI/.venv, otherwise an ImageMagick corner flood fill), trimmed, fitted
inside 96x96 with the aspect kept, and centred on a 128x128 black canvas, so the icon
generator gets a 16 px black margin on every side and the silhouette never clips.

Writes ui/assets/refs/<name>_ref.png; downloads live in tmp/refs/ (both are local
artifacts, not part of the tree).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = os.path.join(ROOT, "tmp", "refs")
OUT = os.path.join(ROOT, "ui", "assets", "refs")
VENV_PY = os.path.expanduser("~/ai-tools/ComfyUI/.venv/bin/python")
UA = "BananaOS-icon-ref/1.0 (https://github.com/jessussaves-dev/Banana-Pocket)"
CANVAS = 128
FIT = 96
# rembg's CLI drags in its server extras (uvicorn and friends); the API is enough
REMBG = ("import sys;from rembg import remove,new_session;from PIL import Image;"
         "im=Image.open(sys.argv[1]);"
         "remove(im,session=new_session('u2netp')).save(sys.argv[2])")


def api(params):
    url = "https://commons.wikimedia.org/w/api.php?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode())


def search(query, limit=8):
    """Commons file titles for a query, most relevant first."""
    d = api({"action": "query", "list": "search", "srsearch": f"{query} filetype:bitmap",
             "srnamespace": "6", "srlimit": str(limit), "format": "json"})
    return [x["title"] for x in d.get("query", {}).get("search", [])]


def thumb_url(title, width=800):
    """A downloadable URL and mime for a File: title (None if it is not a bitmap)."""
    d = api({"action": "query", "titles": title, "prop": "imageinfo",
             "iiprop": "url|mime|size", "iiurlwidth": str(width), "format": "json"})
    for page in d.get("query", {}).get("pages", {}).values():
        for ii in page.get("imageinfo", []):
            mime = ii.get("mime", "")
            if mime in ("image/png", "image/jpeg", "image/webp"):
                return ii.get("thumburl") or ii.get("url"), mime
    return None, None


def download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f)


def background_score(path):
    """Lower is better: an image with transparency or a plain white studio backdrop."""
    ch = subprocess.run(["magick", "identify", "-format", "%[channels]", path],
                        capture_output=True, text=True).stdout
    if "a" in ch:
        return 0.0
    # standard deviation of the top and bottom 6% strips: a studio backdrop is flat
    vals = []
    for geom in ("100%x6%+0+0", "100%x6%+0-0"):
        r = subprocess.run(["magick", path, "-alpha", "off", "-crop", geom, "+repage",
                            "-format", "%[fx:standard_deviation]", "info:"],
                           capture_output=True, text=True)
        try:
            vals.append(float(r.stdout.strip()))
        except ValueError:
            return 1.0
    return sum(vals) / len(vals)


def fetch(query, name):
    os.makedirs(TMP, exist_ok=True)
    best = None
    for title in search(query):
        url, mime = thumb_url(title)
        if not url:
            continue
        cand = os.path.join(TMP, f"cand_{name}.png")
        try:
            download(url, cand)
        except Exception:
            continue
        score = background_score(cand)
        print(f"  {score:5.2f}  {title}")
        if best is None or score < best[0]:
            best = (score, url, title, cand)
        if score < 0.02:               # already transparent or a flat white plate
            break
    if not best:
        sys.exit(f"fetch-ref: no usable Wikimedia Commons file for {query!r}")
    _, url, title, cand = best
    raw = os.path.join(TMP, f"raw_{name}.png")
    shutil.move(cand, raw)
    print(f"  source: {title}  ({url})")
    return raw


def already_cut(path):
    """True when the image already carries an alpha cut-out (an existing icon)."""
    ch = subprocess.run(["magick", "identify", "-format", "%[channels]", path],
                        capture_output=True, text=True).stdout
    if "a" not in ch:
        return False
    r = subprocess.run(["magick", path, "-alpha", "extract", "-format",
                        "%[fx:mean]", "info:"], capture_output=True, text=True)
    try:
        return float(r.stdout.strip()) < 0.95     # corner pixels are transparent
    except ValueError:
        return False


def cut_out(src, dst):
    """rembg when it is installed, otherwise an ImageMagick corner flood fill."""
    if already_cut(src):
        shutil.copyfile(src, dst)
        return "kept (already transparent)"
    have_rembg = os.path.exists(VENV_PY) and subprocess.run(
        [VENV_PY, "-c", "import rembg"], capture_output=True).returncode == 0
    if have_rembg:
        r = subprocess.run([VENV_PY, "-c", REMBG, src, dst],
                           capture_output=True, text=True)
        if r.returncode == 0:
            return "rembg u2netp"
        print(f"  (rembg failed, falling back to flood fill: {r.stderr.strip()[:120]})")
    # corner flood fill + binary alpha, so black outlines inside the sprite survive
    last = subprocess.run(["magick", "identify", "-format", "%w %h", src],
                          capture_output=True, text=True).stdout.split()
    if len(last) != 2:
        sys.exit("fetch-ref: cannot read the image")
    w, h = int(last[0]), int(last[1])
    cmd = ["magick", src, "-alpha", "on", "-fuzz", "12%", "-fill", "none"]
    for x, y in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
        cmd += ["-draw", f"color {x},{y} floodfill"]
    cmd += ["-channel", "A", "-threshold", "50%", "+channel", dst]
    if subprocess.run(cmd, capture_output=True).returncode != 0:
        sys.exit("fetch-ref: the flood fill failed")
    return "flood fill"


def prepare(src, name):
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(TMP, exist_ok=True)
    cut = os.path.join(TMP, f"cut_{name}.png")
    how = cut_out(src, cut)
    out = os.path.join(OUT, f"{name}_ref.png")
    # trim the empty space, fit 96x96 keeping the aspect, centre on a black 128 canvas
    r = subprocess.run(["magick", cut, "-trim", "+repage",
                        "-resize", f"{FIT}x{FIT}",
                        "-background", "#000000", "-gravity", "center",
                        "-extent", f"{CANVAS}x{CANVAS}",
                        "-alpha", "remove", "-alpha", "off", "PNG24:" + out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"fetch-ref: magick failed: {r.stderr.strip()}")
    print(f"{out}  ({how})")
    return out


def main():
    ap = argparse.ArgumentParser(description="Fetch/prepare an icon reference for gen-icon.py --ref.")
    ap.add_argument("--name", required=True, help="slug: dreamcast, gamegear, zxspectrum, ...")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--query", help="Wikimedia Commons search, e.g. 'Sega Dreamcast controller'")
    src.add_argument("--local", help="an existing icon or local photo to cut out")
    a = ap.parse_args()

    if os.sep in a.name:
        sys.exit("fetch-ref: --name is a slug, not a path")
    if a.local:
        if not os.path.isfile(a.local):
            sys.exit(f"fetch-ref: no such file: {a.local}")
        print(f"reference: {a.local}")
        src = a.local
    else:
        print(f"searching Commons for {a.query!r}:")
        src = fetch(a.query, a.name)
    prepare(src, a.name)


if __name__ == "__main__":
    main()
