#!/usr/bin/python3
"""banana.local: the handheld's web page. Games and BIOS in, saves, backups and logs out, updates applied.

Started by `web up` (Settings > Network > Web page). A PIN made at start is shown in the menu; the browser asks for
it once and keeps a cookie. The page wears the active menu theme (palette, accent, font). mDNS answers for
banana.local come from here too: the image has no avahi. Secrets (Wi-Fi passwords, accounts, SSH keys) are never
listed, read or written; the backup is the one file that carries them, and only after the PIN.
  banana-web.py [--port N]   BANANA_DATA / BANANA_RUN / BANANA_SHARE point at other roots (the host test)
"""
import hashlib, hmac, html, json, os, re, secrets, socket, struct, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, unquote, urlparse

DATA = os.environ.get("BANANA_DATA", "/data")
RUN = os.environ.get("BANANA_RUN", "/run/banana")
SHARE = os.environ.get("BANANA_SHARE", "/usr/share/banana")
ETC = os.environ.get("BANANA_ETC", "/etc/banana")
LIBEXEC = os.environ.get("BANANA_LIBEXEC", "/usr/libexec/banana")
NAME = "banana"
SECRET_NAMES = {"wifi.conf", "accounts.conf", "ra.token", "host_key_ed25519", "ssh", "authorized_keys"}
MAX_UPLOAD = 8 << 30
SESSIONS = {}
FAILS = {}
LOCK = threading.Lock()


# ---------------------------------------------------------------- files

def is_secret(path):
    return any(p in SECRET_NAMES for p in path.split("/"))


def safe(rel, root):
    """rel (from the browser) under root, or None: no .., no secrets, no dot files."""
    parts = [p for p in unquote(rel).split("/") if p]
    if any(p in (".", "..") or p.startswith(".") or "\0" in p for p in parts) or is_secret("/".join(parts)):
        return None
    full = os.path.realpath(os.path.join(root, *parts))
    base = os.path.realpath(root)
    return full if full == base or full.startswith(base + os.sep) else None


def read_conf(path):
    kv = {}
    try:
        for line in open(path, errors="replace"):
            line = line.strip()
            if line and line[0] not in "#;" and "=" in line:
                k, v = line.split("=", 1)
                kv[k.strip()] = v.strip()
    except OSError:
        pass
    return kv


def settings():
    kv = read_conf(ETC + "/settings.conf")
    kv.update(read_conf(DATA + "/banana/settings.conf"))
    return kv


def systems():
    out, cur = [], None
    try:
        for line in open(ETC + "/systems.conf", errors="replace"):
            line = line.strip()
            m = re.match(r"\[(.+)\]$", line)
            if m:
                cur = {"id": m.group(1), "name": m.group(1), "ext": ""}
                out.append(cur)
            elif cur is not None and "=" in line and line[0] != "#":
                k, v = [s.strip() for s in line.split("=", 1)]
                if k in ("name", "ext"):
                    cur[k] = v
    except OSError:
        pass
    return out


# ---------------------------------------------------------------- theme

NAMED = {"yellow": "#ffd23f", "blue": "#4aa3ff", "red": "#ff5a4d", "green": "#5ad06a", "purple": "#a06bff", "pink": "#ff6bb5"}
DARK = {"bg": "#17161c", "panel": "#17161c", "band": "#3a3540", "ink": "#f1ede2", "muted": "#a39d8e", "edge": "#e8e2d3", "onpick": "#17161c"}
LIGHT = {"bg": "#f3efe6", "panel": "#f6f2ea", "band": "#ffffff", "ink": "#2a2620", "muted": "#7a7262", "edge": "#3a342c", "onpick": "#17161c"}
COLOR_KEYS = {"screenBg": "bg", "panelBody": "panel", "panelBand": "band", "ink": "ink", "muted": "muted", "edge": "edge", "onPick": "onpick"}


def strip_jsonc(text):
    out, i, n, q = [], 0, len(text), False
    while i < n:
        c = text[i]
        if q:
            out.append(c)
            if c == "\\" and i + 1 < n:
                i += 1
                out.append(text[i])
            elif c == '"':
                q = False
        elif c == '"':
            q = True
            out.append(c)
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    return re.sub(r",(\s*[}\]])", r"\1", "".join(out))


def theme_file(value):
    if value.startswith("user:"):
        for d in (DATA + "/banana/themes", SHARE + "/themes"):
            p = os.path.join(d, os.path.basename(value[5:]) + ".json")
            if os.path.isfile(p):
                return p
    elif value.startswith("file:") and os.path.isfile(value[5:]):
        return value[5:]
    return None


def hexcolor(v):
    return v if isinstance(v, str) and re.fullmatch(r"#[0-9a-fA-F]{6}", v) else None


def theme():
    """The active theme as page colours: {vars..., style, font, name}. Pocket (no file) is the dark palette."""
    value = settings().get("theme", "")
    pal, style, font, accent, name = dict(DARK), "classic", "inter", "#ffd23f", "Pocket"
    if value == "light":
        pal = dict(LIGHT)
    path = theme_file(value)
    if path:
        try:
            t = json.loads(strip_jsonc(open(path, errors="replace").read()))
        except ValueError:
            t = {}
        if t.get("base") == "light":
            pal = dict(LIGHT)
        a = t.get("accent")
        if isinstance(a, dict):
            a = a.get("base")
        accent = NAMED.get(a) or hexcolor(a) or accent
        for k, dst in COLOR_KEYS.items():
            c = hexcolor((t.get("colors") or {}).get(k))
            if c:
                pal[dst] = c
        style = t.get("style") or style
        font = t.get("font") or font
        name = t.get("name") or name
    pal["accent"] = accent
    pal["style"], pal["font"], pal["name"] = style, font, name
    return pal


FONTS = {"banana": ("BananaPixel.ttf", "BananaTitle.ttf"), "banana-blast": ("BananaBlast.ttf", "BananaBlastTitle.ttf"),
         "inter": ("Inter-Regular.ttf", "Inter-Bold.ttf"), "pixel": ("Inter-Regular.ttf", "Inter-Bold.ttf")}


def css():
    t = theme()
    body, title = FONTS.get(t["font"], FONTS["inter"])
    pixel = t["style"] in ("snes", "blast") or t["font"] in ("banana", "banana-blast")
    return f"""@font-face{{font-family:bb;src:url(/font/{body})}}@font-face{{font-family:bt;src:url(/font/{title})}}
:root{{--bg:{t['bg']};--panel:{t['panel']};--band:{t['band']};--ink:{t['ink']};--muted:{t['muted']};--edge:{t['edge']};
--accent:{t['accent']};--onpick:{t['onpick']};--r:{'0' if pixel else '12px'};--bw:{'3px' if pixel else '1px'}}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--ink);font:{'16px' if pixel else '15px/1.5'} bb,system-ui,sans-serif}}
{'body{image-rendering:pixelated}' if pixel else ''}
header{{background:var(--band);border-bottom:var(--bw) solid var(--accent);padding:14px 16px}}
h1,h2{{font-family:bt,bb,system-ui,sans-serif;margin:0}}h1{{font-size:22px;color:var(--accent)}}h2{{font-size:18px;margin:0 0 10px}}
header small{{color:var(--muted)}}main{{max-width:760px;margin:0 auto;padding:16px;display:grid;gap:16px}}
section{{background:var(--panel);border:var(--bw) solid var(--edge);border-radius:var(--r);padding:14px}}
a{{color:var(--accent)}}small,.m{{color:var(--muted)}}
button,input[type=submit],.btn{{background:var(--accent);color:var(--onpick);border:var(--bw) solid var(--edge);border-radius:var(--r);
padding:8px 14px;font:inherit;cursor:pointer;text-decoration:none;display:inline-block}}
input,select{{background:var(--bg);color:var(--ink);border:var(--bw) solid var(--edge);border-radius:var(--r);padding:8px;font:inherit;max-width:100%}}
table{{width:100%;border-collapse:collapse}}td{{padding:5px 4px;border-top:1px solid var(--muted);word-break:break-all}}
td:last-child{{text-align:right;white-space:nowrap}}pre{{white-space:pre-wrap;word-break:break-all;max-height:320px;overflow:auto;background:var(--bg);padding:8px}}
progress{{width:100%;accent-color:var(--accent)}}.row{{display:flex;gap:8px;flex-wrap:wrap;align-items:center}}"""


# ---------------------------------------------------------------- pages

def esc(s):
    return html.escape(str(s), quote=True)


def page(body, title="Banana Pocket"):
    return f"""<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>{esc(title)}</title><link rel=stylesheet href=/style.css></head><body>
<header><h1>Banana Pocket</h1><small>{esc(theme()['name'])}</small></header><main>{body}</main></body></html>"""


def login_page(msg=""):
    return page(f"""<section><h2>PIN</h2><p>Type the PIN shown on the handheld (Settings &gt; Network &gt; Web page).</p>
<p class=m>{esc(msg)}</p><form method=post action=/login class=row>
<input name=pin inputmode=numeric autocomplete=off autofocus size=8 maxlength=8><input type=submit value=Open></form></section>""")


UPLOAD_JS = """<script>
function up(f){f.preventDefault();var form=f.target,fs=form.querySelector('[type=file]').files,bar=form.querySelector('progress'),
out=form.querySelector('.out'),i=0;if(!fs.length)return;(function next(){if(i>=fs.length){out.textContent='Done: '+fs.length+' file(s).';
if(form.dataset.reload)location.reload();return}var x=new XMLHttpRequest(),fd=new FormData(),n=fs[i];
x.open('POST',form.action+(form.action.indexOf('?')<0?'?':'&')+'name='+encodeURIComponent(n.name));
x.upload.onprogress=function(e){if(e.lengthComputable){bar.value=e.loaded/e.total}};
x.onload=function(){if(x.status!=200){out.textContent=n.name+': '+x.responseText;return}i++;next()};
x.onerror=function(){out.textContent='Upload failed'};out.textContent=n.name+'...';x.send(n)})()}
</script>"""


def home():
    s = systems()
    opts = "".join(f'<option value="{esc(x["id"])}">{esc(x["name"])}</option>' for x in s)
    logs = "".join(f'<tr><td><a href="/logs/{quote(n)}">{esc(n)}</a></td></tr>' for n in list_dir(DATA + "/banana/logs"))
    ups = "".join(f'<tr><td>{esc(n)}</td><td><form method=post action="/update/apply" style="display:inline">'
                  f'<input type=hidden name=name value="{esc(n)}"><input type=submit value="Install"></form></td></tr>'
                  for n in list_dir(DATA + "/banana/updates") if n.endswith(".tar"))
    bios = "".join(f"<tr><td>{esc(n)}</td></tr>" for n in list_dir(DATA + "/bios"))
    return page(f"""{UPLOAD_JS}
<section><h2>Games</h2><p class=m>Pick the system, then the files. They land in roms/&lt;system&gt;. Press Y in the menu to rescan.</p>
<form onsubmit="up(event)" action="/upload/roms" data-reload=0><div class=row>
<select name=system onchange="this.form.action='/upload/roms/'+this.value">{opts}</select>
<input type=file multiple><input type=submit value=Send></div><progress value=0 max=1></progress><div class=out></div></form>
<script>document.currentScript.previousElementSibling.querySelector('select').dispatchEvent(new Event('change'))</script></section>
<section><h2>BIOS</h2><form onsubmit="up(event)" action="/upload/bios" data-reload=1><div class=row><input type=file multiple>
<input type=submit value=Send></div><progress value=0 max=1></progress><div class=out></div></form><table>{bios}</table></section>
<section><h2>Saves and backup</h2><p class=m>Saves and states as one zip; the backup also carries settings and Wi-Fi passwords, so keep it private.</p>
<div class=row><a class=btn href=/download/saves.zip>Saves + states (zip)</a><form method=post action=/backup/create style="display:inline">
<input type=submit value="Make a backup"></form></div><table>{"".join(f'<tr><td>{esc(n)}</td><td><a href="/backup/get/{quote(n)}">Download</a></td></tr>' for n in list_dir(DATA + "/banana/backups"))}</table></section>
<section><h2>Logs</h2><table>{logs}</table></section>
<section><h2>Update</h2><p class=m>Send a banana-*.update.tar; it is checked against its signature before it is used.</p>
<form onsubmit="up(event)" action="/upload/update" data-reload=1><div class=row><input type=file accept=.tar><input type=submit value=Send></div>
<progress value=0 max=1></progress><div class=out></div></form><table>{ups}</table><p class=m>{esc(update_status())}</p></section>""")


def list_dir(d):
    try:
        return sorted((n for n in os.listdir(d) if not n.startswith(".")), key=str.lower)
    except OSError:
        return []


def update_status():
    try:
        return open(RUN + "/update-status").read().strip()
    except OSError:
        return ""


# ---------------------------------------------------------------- server

class H(BaseHTTPRequestHandler):
    server_version = "banana"
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body=b"", ctype="text/html; charset=utf-8", extra=()):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def redirect(self, to, extra=()):
        self.send(303, b"", extra=(("Location", to),) + tuple(extra))

    def authed(self):
        for c in self.headers.get("Cookie", "").split(";"):
            k, _, v = c.strip().partition("=")
            if k == "bs" and v in SESSIONS and SESSIONS[v] > time.time():
                return True
        return False

    def same_origin(self):
        o = self.headers.get("Origin")
        return not o or urlparse(o).netloc == self.headers.get("Host")

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        if p == "/style.css":
            return self.send(200, css(), "text/css")
        if p.startswith("/font/"):
            return self.font(p[6:])
        if not self.authed():
            return self.send(200, login_page())
        if p == "/":
            return self.send(200, home())
        if p == "/download/saves.zip":
            return self.zip_saves()
        if p.startswith("/logs/"):
            f = safe(p[6:], DATA + "/banana/logs")
            if f and os.path.isfile(f):
                with open(f, "rb") as fh:
                    fh.seek(max(0, os.path.getsize(f) - 200000))
                    txt = fh.read().decode(errors="replace")
                return self.send(200, page(f"<section><h2>{esc(os.path.basename(f))}</h2><pre>{esc(txt)}</pre></section>"))
        if p.startswith("/backup/get/"):
            f = safe(p[12:], DATA + "/banana/backups")
            if f and os.path.isfile(f):
                return self.file(f, "application/gzip", True)
        self.send(404, page("<section>Not found.</section>"))

    def font(self, name):
        name = os.path.basename(name)
        f = os.path.join(SHARE, "banana/fonts", name)
        if name.endswith(".ttf") and os.path.isfile(f):
            return self.file(f, "font/ttf", False)
        self.send(404)

    def file(self, f, ctype, attach):
        size = os.path.getsize(f)
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        if attach:
            self.send_header("Content-Disposition", f'attachment; filename="{os.path.basename(f)}"')
        self.end_headers()
        with open(f, "rb") as fh:
            while True:
                b = fh.read(1 << 16)
                if not b:
                    break
                self.wfile.write(b)

    def zip_saves(self):
        # a zip of saves/ and states/, streamed from a child so a big card doesn't fill RAM
        self.send_response(200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Disposition", 'attachment; filename="banana-saves.zip"')
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        pr = subprocess.Popen([sys.executable, "-c", ZIP_CHILD, DATA], stdout=subprocess.PIPE)
        try:
            while True:
                b = pr.stdout.read(1 << 16)
                if not b:
                    break
                self.wfile.write(b)
        finally:
            pr.stdout.close()
            pr.wait()

    def do_POST(self):
        u = urlparse(self.path)
        p = u.path
        if not self.same_origin():
            return self.send(403, "cross-site request refused", "text/plain")
        n = int(self.headers.get("Content-Length") or 0)
        if p == "/login":
            return self.login(self.rfile.read(min(n, 4096)).decode(errors="replace"))
        if not self.authed():
            return self.send(401, "PIN needed", "text/plain")
        if p.startswith("/upload/"):
            return self.upload(p[8:], parse_qs(u.query).get("name", [""])[0], n)
        form = parse_qs(self.rfile.read(min(n, 65536)).decode(errors="replace"))
        if p == "/backup/create":
            subprocess.run([LIBEXEC + "/backup", "create"], capture_output=True)
            return self.redirect("/")
        if p == "/update/apply":
            f = safe(form.get("name", [""])[0], DATA + "/banana/updates")
            if f and os.path.isfile(f) and f.endswith(".tar"):
                # the update tool reports through /run/banana/update-status, which the page shows
                subprocess.Popen([LIBEXEC + "/update", "apply", f], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)
                return self.redirect("/")
        self.send(404, "not found", "text/plain")

    def login(self, body):
        ip = self.client_address[0]
        with LOCK:
            tries = [t for t in FAILS.get(ip, []) if t > time.time() - 60]
            if len(tries) >= 5:
                return self.send(429, login_page("Too many tries; wait a minute."))
        pin = parse_qs(body).get("pin", [""])[0].strip()
        if hmac.compare_digest(pin.encode(), PIN.encode()):
            tok = secrets.token_hex(16)
            with LOCK:
                SESSIONS[tok] = time.time() + 86400
            return self.redirect("/", (("Set-Cookie", f"bs={tok}; Path=/; HttpOnly; SameSite=Strict"),))
        with LOCK:
            FAILS[ip] = tries + [time.time()]
        self.send(200, login_page("Wrong PIN."))

    def upload(self, kind, name, n):
        name = os.path.basename(unquote(name))
        if not name or name.startswith(".") or is_secret(name) or n <= 0 or n > MAX_UPLOAD:
            self.rfile.read(0)
            return self.send(400, "bad file name or size", "text/plain")
        if kind.startswith("roms/"):
            sid = kind[5:]
            if sid not in [s["id"] for s in systems()]:
                return self.send(400, "unknown system", "text/plain")
            dest = DATA + "/roms/" + sid
        elif kind == "bios":
            dest = DATA + "/bios"
        elif kind == "update":
            if not name.endswith(".tar"):
                return self.send(400, "an update is a .tar", "text/plain")
            dest = DATA + "/banana/updates"
        else:
            return self.send(404, "not found", "text/plain")
        try:
            os.makedirs(dest, exist_ok=True)
            tmp = os.path.join(dest, "." + name + ".part")
            left = n
            with open(tmp, "wb") as fh:
                while left:
                    b = self.rfile.read(min(left, 1 << 16))
                    if not b:
                        raise OSError("connection dropped")
                    fh.write(b)
                    left -= len(b)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(tmp, os.path.join(dest, name))
        except OSError as e:
            try:
                os.unlink(tmp)
            except OSError:
                pass
            self.close_connection = True
            return self.send(500, f"write failed: {e}", "text/plain")
        self.send(200, "ok", "text/plain")


ZIP_CHILD = r"""
import os, sys, zipfile
class W:
    def __init__(s): s.o = sys.stdout.buffer; s.p = 0
    def write(s, b): s.p += len(b); return s.o.write(b)
    def flush(s): s.o.flush()
    def tell(s): return s.p
data = sys.argv[1]
with zipfile.ZipFile(W(), "w", zipfile.ZIP_STORED) as z:
    for top in ("saves", "states"):
        for r, _, fs in os.walk(os.path.join(data, top)):
            for f in fs:
                p = os.path.join(r, f)
                if os.path.isfile(p):
                    z.write(p, os.path.relpath(p, data))
"""


# ---------------------------------------------------------------- mDNS

def my_ips():
    ips = []
    try:
        out = subprocess.run(["ip", "-4", "-o", "addr"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            f = line.split()
            if f[1] != "lo" and not f[3].startswith("169.254.170."):
                ips.append((f[1], f[3].split("/")[0]))
    except OSError:
        pass
    return ips


def mdns():
    """Answer A queries for banana.local with the address of the interface the query came in on."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", 5353))
        for _, ip in my_ips():
            try:
                s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton("224.0.0.251") + socket.inet_aton(ip))
            except OSError:
                pass
    except OSError:
        return
    want = b"\x06" + NAME.encode() + b"\x05local\x00"
    while True:
        try:
            data, src = s.recvfrom(1500)
        except OSError:
            return
        if len(data) < 12 or data[2] & 0x80 or want not in data[12:]:
            continue
        ips = my_ips()
        if not ips:
            continue
        # the address on the subnet of the asker, else the first
        ip = next((a for _, a in ips if a.rsplit(".", 1)[0] == src[0].rsplit(".", 1)[0]), ips[0][1])
        resp = struct.pack(">HHHHHH", 0, 0x8400, 0, 1, 0, 0) + want + struct.pack(">HHIH", 1, 0x8001, 120, 4) + socket.inet_aton(ip)
        try:
            s.sendto(resp, ("224.0.0.251", 5353))
            s.sendto(resp, src)
        except OSError:
            pass


# ---------------------------------------------------------------- main

PIN = ""


def main():
    global PIN
    port = int(sys.argv[sys.argv.index("--port") + 1]) if "--port" in sys.argv else 80
    PIN = "%06d" % secrets.randbelow(10 ** 6)
    os.makedirs(RUN, exist_ok=True)
    tmp = RUN + "/web.pin.new"
    with open(tmp, "w") as f:
        f.write(PIN + "\n")
    os.replace(tmp, RUN + "/web.pin")
    threading.Thread(target=mdns, daemon=True).start()
    srv = ThreadingHTTPServer(("", port), H)
    srv.daemon_threads = True
    srv.serve_forever()


if __name__ == "__main__":
    main()
