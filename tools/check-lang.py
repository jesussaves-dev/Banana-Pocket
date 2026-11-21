#!/usr/bin/env python3
# The bundled language catalogs (ui/assets/lang), on the host.
#
#   tools/check-lang.py [repo root]     coverage per language, then any problems; nonzero if there were any
#
# A catalog's keys are the English strings the menu passes to tr(); a value is the translation. So:
#   - every key in es/de must exist in en (a typo or a removed string is an error);
#   - every en key must still appear in ui/src (reworded English would otherwise silently fall back forever);
#   - a missing translation is coverage, not a failure (the menu falls back to English);
#   - a catalog must have a "name" and string values, or the row and the loader would show nothing.
import glob, importlib.util, json, os, re, sys

def theme_texts(root):
    """Every translatable "text" literal in the shipped themes: not "=expr" code, and with some prose outside its {placeholders}."""
    spec = importlib.util.spec_from_file_location("check_themes", os.path.join(root, "tools", "check-themes.py"))
    ct = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ct)
    out = {}
    def walk(o, f):
        if isinstance(o, dict):
            for k, v in o.items():
                if k == "text" and isinstance(v, str) and not v.startswith("=") and re.search(r"[A-Za-z]{2}", re.sub(r"\{[^}]*\}", "", v)):
                    out.setdefault(v, f)
                walk(v, f)
        elif isinstance(o, list):
            for v in o:
                walk(v, f)
    for f in glob.glob(os.path.join(root, "system", "rootfs", "usr", "share", "banana", "themes", "**", "*.json"), recursive=True):
        try:
            with open(f, encoding="utf-8") as fh:
                walk(json.loads(ct.strip_comments(fh.read())), os.path.basename(f))
        except ValueError:
            pass    # check-themes reports unparsable themes
    return out

def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)

def main(argv):
    root = next((a for a in argv[1:] if not a.startswith("-")), os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    langdir = os.path.join(root, "ui", "assets", "lang")
    src = "".join(open(p, encoding="utf-8").read() for p in glob.glob(os.path.join(root, "ui", "src", "*.cpp")) + glob.glob(os.path.join(root, "ui", "src", "*.h")))
    files = sorted(glob.glob(os.path.join(langdir, "*.json")))
    if not files:
        print("check-lang: no catalogs in %s" % langdir)
        return 2
    themes = theme_texts(root)
    errors, catalogs = [], {}
    for path in files:
        code = os.path.splitext(os.path.basename(path))[0]
        try:
            doc = load(path)
        except (OSError, ValueError) as e:
            errors.append("%s: %s" % (code, e)); continue
        name, strings = doc.get("name"), doc.get("strings")
        if not isinstance(name, str) or not name:
            errors.append("%s: no \"name\"" % code)
        if not isinstance(strings, dict):
            errors.append("%s: no \"strings\" object" % code); continue
        for k, v in strings.items():
            if not isinstance(v, str) or not v.strip():
                errors.append("%s: %r has no text" % (code, k))
        catalogs[code] = (name, strings)
    if "en" not in catalogs:
        print("check-lang: no en.json (the reference list of strings)")
        return 2
    en = catalogs["en"][1]
    for k, v in en.items():
        if v != k:
            errors.append("en: %r maps to %r; en.json must map each key to itself" % (k, v))
        if k not in src and k.replace('"', '\\"') not in src and k not in themes:
            errors.append("en: %r is not in ui/src any more (reworded? update the catalogs)" % k)
    for code, (name, strings) in sorted(catalogs.items()):
        if code == "en":
            continue
        extra = [k for k in strings if k not in en]
        for k in extra:
            errors.append("%s: %r is not an English string in en.json" % (code, k))
        missing = [k for k in en if k not in strings]
        print("  %-3s %-10s %3d/%3d strings%s" % (code, name, len(strings), len(en),
            "" if not missing else "  (%d still English)" % len(missing)))
    # a literal handed to tr() has to be a key, or that row can never be translated. This is the check that was
    # missing when the update and box-art values shipped English-only: they were composed in C++ and only reached
    # tr() as a variable, so nothing could see them. (The lookbehind keeps str("id")-style accessors out.)
    for lit in sorted(set(re.findall(r'(?<![A-Za-z0-9_])tr\(\s*"((?:[^"\\]|\\.)*)"', src))):
        if lit not in en:
            errors.append("ui/src: tr(\"%s\") is not a key in en.json" % lit)
    for lit, f in sorted(themes.items()):
        if lit not in en:
            errors.append("themes/%s: text \"%s\" is not a key in en.json (layout runs a text node through tr())" % (f, lit))
    for e in errors:
        print("FAIL " + e)
    print("check-lang: %d catalog(s), %d problem(s)" % (len(catalogs), len(errors)))
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
