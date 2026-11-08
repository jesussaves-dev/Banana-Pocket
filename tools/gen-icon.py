#!/usr/bin/env python3
"""Generate a 32x32 pixel-art icon for BananaOS with a local, headless ComfyUI.

    python3 tools/gen-icon.py --prompt "dreamcast console, swirl logo" --name dreamcast
    python3 tools/gen-icon.py --prompt "game boy advance" --name gba --colors 12 --seed 42

It talks to ComfyUI on 127.0.0.1:8188 and starts it (--lowvram, headless) if it is not
running, renders with SDXL + the Pixel Art XL LoRA at 4 steps (SDXL Lightning), then
cuts the background away from the edges, snaps the icon to 32x32 and quantises it with
ImageMagick.

The result goes to ui/assets/overrides/icons/<name>.png at 128x128, the size the menu's
own override icons use -- never to ui/assets/rnds/icons (the vendored engine's art).
"""
import argparse
import json
import os
import random
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

COMFY_DIR = os.path.expanduser("~/ai-tools/ComfyUI")
COMFY_URL = "http://127.0.0.1:8188"
CHECKPOINT = "sdxl-base-1.0.safetensors"
LORA = "pixel-art-xl.safetensors"
LIGHTNING = "sdxl_lightning_4step_lora.safetensors"
PROMPT = ("pixel art icon of {prompt}, chunky 16-bit game icon, flat solid colours, "
          "thick black outline, limited palette, centered, plain solid black "
          "background #000000, uniform flat background, no shadow, no gradient, "
          "no dithering, crisp edges")
NEGATIVE = ("gradient, shadow, floor, vignette, blurry, photo, text, watermark, "
            "multiple objects, realistic, noise, dithering, anti-aliasing, soft "
            "shading, 3d render, badge, emblem, circular frame, colored plate, "
            "background ring, border, sticker, pedestal")
SIZE = 1024          # the render
OUT_SIZE = 128       # what ui/assets/overrides/icons holds

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "ui", "assets", "overrides", "icons")


def http_json(path, data=None, timeout=30):
    req = urllib.request.Request(COMFY_URL + path)
    body = None
    if data is not None:
        body = json.dumps(data).encode()
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, body, timeout=timeout) as r:
        return json.loads(r.read().decode())


def comfy_up(timeout=2):
    try:
        http_json("/system_stats", timeout=timeout)
        return True
    except (urllib.error.URLError, OSError, ValueError):
        return False


def start_comfy():
    """Start ComfyUI headless in its own session and wait for the API."""
    py = os.path.join(COMFY_DIR, ".venv", "bin", "python")
    if not os.path.exists(py):
        sys.exit(f"gen-icon: no ComfyUI at {COMFY_DIR} (expected {py}). See AGENTS.md.")
    log = os.path.join(COMFY_DIR, "comfyui.log")
    with open(log, "ab") as lf:
        subprocess.Popen(
            [py, "main.py", "--listen", "127.0.0.1", "--port", "8188",
             "--lowvram", "--disable-auto-launch"],
            cwd=COMFY_DIR, stdout=lf, stderr=lf, stdin=subprocess.DEVNULL,
            start_new_session=True)
    for _ in range(180):
        if comfy_up():
            return
        time.sleep(1)
    sys.exit(f"gen-icon: ComfyUI did not come up on {COMFY_URL}; see {log}")


def workflow(prompt, negative, seed, steps=24, cfg=6.0, lora=0.6,
             ref_name=None, denoise=0.60):
    """SDXL + Pixel Art XL + Lightning, in the API's node format.

    ref_name: an image already copied into ComfyUI/input; it is VAE-encoded as the
    starting latent (img2img), so a real console's silhouette survives the restyle.
    """
    wf = {
        "1": {"class_type": "CheckpointLoaderSimple",
              "inputs": {"ckpt_name": CHECKPOINT}},
        "2": {"class_type": "LoraLoader",
              "inputs": {"model": ["1", 0], "clip": ["1", 1], "lora_name": LORA,
                         "strength_model": lora, "strength_clip": lora}},
        "9": {"class_type": "LoraLoader",
              "inputs": {"model": ["2", 0], "clip": ["2", 1], "lora_name": LIGHTNING,
                         "strength_model": 1.0, "strength_clip": 1.0}},
        "3": {"class_type": "CLIPTextEncode",
              "inputs": {"text": prompt, "clip": ["9", 1]}},
        "4": {"class_type": "CLIPTextEncode",
              "inputs": {"text": negative, "clip": ["9", 1]}},
        "5": {"class_type": "EmptyLatentImage",
              "inputs": {"width": SIZE, "height": SIZE, "batch_size": 1}},
        "6": {"class_type": "KSampler",
              "inputs": {"seed": seed, "steps": steps, "cfg": cfg,
                         "sampler_name": "euler", "scheduler": "sgm_uniform",
                         "denoise": 1.0, "model": ["9", 0], "positive": ["3", 0],
                         "negative": ["4", 0], "latent_image": ["5", 0]}},
        "7": {"class_type": "VAEDecode", "inputs": {"samples": ["6", 0], "vae": ["1", 2]}},
        "8": {"class_type": "SaveImage",
              "inputs": {"filename_prefix": "banana-icon", "images": ["7", 0]}},
    }
    if ref_name:
        wf["0"] = {"class_type": "LoadImage", "inputs": {"image": ref_name}}
        wf["10"] = {"class_type": "VAEEncode",
                    "inputs": {"pixels": ["0", 0], "vae": ["1", 2]}}
        wf["6"]["inputs"]["latent_image"] = ["10", 0]
        wf["6"]["inputs"]["denoise"] = denoise
        del wf["5"]                       # no empty latent when starting from an image
    return wf


def prepare_ref(path):
    """The reference as SDXL sees it: alpha onto black, 86% inside a 128 square, then
    1024 on a point grid. Returns the file name inside ComfyUI/input."""
    name = "banana-ref-" + uuid.uuid4().hex[:8] + ".png"
    dest = os.path.join(COMFY_DIR, "input", name)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    cmd = ["magick", path, "-background", "black", "-alpha", "remove", "-alpha", "off",
           "-filter", "Point", "-resize", f"{(OUT_SIZE * 86) // 100}x{(OUT_SIZE * 86) // 100}",
           "-gravity", "center", "-background", "black",
           "-extent", f"{OUT_SIZE}x{OUT_SIZE}",
           "-filter", "Point", "-resize", f"{SIZE}x{SIZE}",
           dest]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"gen-icon: cannot read the reference: {r.stderr.strip()}")
    return name


def render(prompt, negative, seed, steps, cfg, lora, ref_name=None, denoise=0.60):
    """Queue the workflow and return the PNG bytes of the first output image."""
    client = str(uuid.uuid4())
    queued = http_json("/prompt", {"prompt": workflow(prompt, negative, seed, steps, cfg, lora,
                                                          ref_name, denoise),
                                   "client_id": client})
    pid = queued.get("prompt_id")
    if not pid:
        sys.exit(f"gen-icon: ComfyUI refused the workflow: {queued}")
    for _ in range(600):                      # SDXL on 6 GB with --lowvram is tens of seconds
        hist = http_json(f"/history/{pid}")
        entry = hist.get(pid)
        if entry:
            status = entry.get("status", {})
            if status.get("status_str") == "error":
                sys.exit(f"gen-icon: the render failed: {json.dumps(status)[:400]}")
            for out in entry.get("outputs", {}).values():
                for img in out.get("images", []):
                    q = urllib.parse.urlencode({"filename": img["filename"],
                                                "subfolder": img.get("subfolder", ""),
                                                "type": img.get("type", "output")})
                    with urllib.request.urlopen(f"{COMFY_URL}/view?{q}", timeout=60) as r:
                        return r.read()
        time.sleep(1)
    sys.exit("gen-icon: timed out waiting for the render")


def post_process(src_png, name, colors, out_dir=OUT_DIR):
    """Cut the background from the edges, 128x128 on a point grid, quantised, RGBA PNG."""
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, name + ".png")
    tmp = out + ".raw.png"
    with open(tmp, "wb") as f:
        f.write(src_png)
    # Background: the prompt asks for a flat magenta plate, keyed out with a generous
    # fuzz; a flood-fill from the corners catches the rest. (The icon's own black
    # outline is never the key colour, so it survives.)
    last = SIZE - 1
    # fuzz first, then one draw chain: a flood-fill from each corner
    # Flood-fill from a ring of border points, so a background that came out as a soft
    # gradient still goes (one corner is not enough), and the icon's own black outline
    # survives: the fill only removes the connected background from the edges inwards.
    seeds = [(x, 0) for x in range(0, SIZE, SIZE // 8)] + [(x, last) for x in range(0, SIZE, SIZE // 8)]
    seeds += [(0, y) for y in range(0, SIZE, SIZE // 8)] + [(last, y) for y in range(0, SIZE, SIZE // 8)]
    cmd = (["magick", tmp, "-alpha", "on", "-fuzz", "20%", "-fill", "none"]
           + sum([["-draw", f"color {x},{y} floodfill"] for x, y in seeds], [])
           + ["-channel", "A", "-threshold", "50%", "+channel",
              "-trim", "+repage",
              "-filter", "Point", "-resize", f"{OUT_SIZE - 8}x{OUT_SIZE - 8}",
              "+dither", "-colors", str(colors),
              "-background", "none", "-gravity", "center",
              "-extent", f"{OUT_SIZE}x{OUT_SIZE}",
              "-alpha", "on", "PNG32:" + out])
    r = subprocess.run(cmd, capture_output=True, text=True)
    os.unlink(tmp)
    if r.returncode != 0:
        sys.exit(f"gen-icon: magick failed: {r.stderr.strip()}")
    return out


def main():
    ap = argparse.ArgumentParser(description="Generate a 128x128 BananaOS pixel-art icon.")
    ap.add_argument("--prompt", required=True, help="what the icon shows, e.g. 'dreamcast console'")
    ap.add_argument("--name", required=True, help="output file name, without .png")
    ap.add_argument("--colors", type=int, default=16, help="palette size (default 16)")
    ap.add_argument("--seed", type=int, default=None, help="fixed seed for a repeatable icon")
    ap.add_argument("--out-dir", default=OUT_DIR, help="where to write (default ui/assets/overrides/icons)")
    ap.add_argument("--steps", type=int, default=24, help="sampler steps")
    ap.add_argument("--cfg", type=float, default=6.0, help="guidance")
    ap.add_argument("--lora", type=float, default=0.6, help="Pixel Art XL strength (0..1)")
    ap.add_argument("--ref", help="reference image (img2img): a real console's silhouette survives")
    ap.add_argument("--denoise", type=float, default=0.60, help="img2img strength (0..1, default 0.60)")
    a = ap.parse_args()

    if os.sep in a.name or a.name in ("", ".", ".."):
        sys.exit("gen-icon: --name is a file name, not a path")
    if not 2 <= a.colors <= 256:
        sys.exit("gen-icon: --colors is 2..256")

    if not 0.0 < a.denoise <= 1.0:
        sys.exit("gen-icon: --denoise is 0..1")
    if a.ref and not os.path.isfile(a.ref):
        sys.exit(f"gen-icon: no reference at {a.ref}")

    seed = a.seed if a.seed is not None else random.randint(0, 2**31 - 1)
    prompt = PROMPT.format(prompt=a.prompt)
    if not comfy_up():
        start_comfy()
    ref_name = prepare_ref(a.ref) if a.ref else None
    png = render(prompt, NEGATIVE, seed, a.steps, a.cfg, a.lora, ref_name, a.denoise)
    out = post_process(png, a.name, a.colors, a.out_dir)
    print(f"{out}  (seed {seed})")


if __name__ == "__main__":
    main()
