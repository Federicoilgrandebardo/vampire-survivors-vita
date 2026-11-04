#!/usr/bin/env python3
"""Builds the trophy pack from the game's achievements: one trophy per achievement, plus the platinum, over two sets.

uv run --with pillow tools/make_trophies.py <extracted game data>
Writes sce_sys/trophy/<title>_00 and _01 and src/trophy_ids.h. Not needed to build the game.
"""
import hashlib
import io
import json
import struct
import sys
from pathlib import Path
from xml.sax.saxutils import escape

from PIL import Image

TITLE = "VSPH00001"
NAME = "Vampire Survivors"
DETAIL = "Hold out against the night and survive until dawn."
SET0 = 127
ICON = 240  # the sizes the system expects: trophies 240x240 RGBA, game icon 320x176 RGB
BG = (40, 12, 18, 255)
SIGN = "<!--Sce-Np-Trophy-Signature: " + "x" * 320 + "-->\n"


def grade(a):
    """B by default; S for what opens new content; G for the arcanas' and the game's big milestones."""
    d = a["description"].lower()
    if "reaper" in d or a.get("arcanaToUnlock") or a.get("stageToUnlock") or a.get("hyperToUnlock"):
        return "G"
    if a.get("characterToUnlock") or a.get("relicToUnlock") or "evolve" in d:
        return "S"
    return "B"


def title(desc):
    """Achievements have no names: the first words of the description, up to 48 characters."""
    s = desc.split("\n")[0].rstrip(". ")
    if len(s) <= 48:
        return s
    return s[:48].rsplit(" ", 1)[0] + "..."


class Atlases:
    def __init__(self, img):
        self.img, self.cache = img, {}

    def frame(self, atlas, name):
        if atlas not in self.cache:
            try:
                self.cache[atlas] = (Image.open(self.img / (atlas + ".png")).convert("RGBA"),
                                     json.loads((self.img / (atlas + ".json")).read_text(encoding="utf-8")))
            except OSError:
                self.cache[atlas] = None
        c = self.cache[atlas]
        if not c:
            return None
        im, j = c
        frames = j.get("frames")
        if frames is None:
            frames = [f for t in j.get("textures", []) for f in t["frames"]]
        if isinstance(frames, dict):
            f = frames.get(name)
        else:
            f = next((x for x in frames if x.get("filename") == name), None)
        if not f:
            return None
        r = f["frame"]
        return im.crop((r["x"], r["y"], r["x"] + r["w"], r["y"] + r["h"]))


def lookup(a, data, atlases):
    w, c, it = a.get("weaponToUnlock"), a.get("characterToUnlock"), a.get("relicToUnlock") or a.get("powerUpToUnlock")
    if w and w in data["weapons"]:
        d = data["weapons"][w][0]
        return atlases.frame(d.get("texture"), d.get("frameName"))
    if c and c in data["characters"]:
        d = data["characters"][c][0]
        return atlases.frame(d.get("textureName"), d.get("spriteName"))
    if a.get("weaponIcon") and a["weaponIcon"] in data["weapons"]:
        d = data["weapons"][a["weaponIcon"]][0]
        return atlases.frame(d.get("texture"), d.get("frameName"))
    if a.get("arcanaToUnlock") is not None:
        v = a["arcanaToUnlock"]
        d = data["arcana"].get(str(v))
        if d is None and isinstance(v, str) and v[:1] == "T" and v[1:3].isdigit():
            d = data["arcana"].get(str(int(v[1:3])))
        d = d[0] if isinstance(d, list) else d
        if d:
            return atlases.frame(d.get("texture"), d.get("frameName"))
    st = a.get("stageToUnlock") or a.get("hyperToUnlock")
    if st and st in data["stages"]:
        d = data["stages"][st][0]
        return atlases.frame(d.get("uiTexture", "UI"), d.get("frameName"))
    if it:
        d = data["items"].get(it) or data["powerups"].get(it)
        d = d[0] if isinstance(d, list) else d
        if d:
            return atlases.frame(d.get("texture", "items"), d.get("frameName"))
    return None


def icon_png(sprite):
    out = Image.new("RGBA", (ICON, ICON), BG)
    if sprite:
        k = max(1, min((ICON - 48) // max(sprite.width, 1), (ICON - 48) // max(sprite.height, 1)))
        s = sprite.resize((sprite.width * k, sprite.height * k), Image.NEAREST)
        out.alpha_composite(s, ((ICON - s.width) // 2, (ICON - s.height) // 2))
    b = io.BytesIO()
    out.save(b, "PNG", optimize=True)
    return b.getvalue()


def trp(files):
    """TROPHY.TRP: 0x40 header, 64-byte entries, then the files; the SHA-1 covers it all with its own field zeroed."""
    base = 0x40 + 64 * len(files)
    entries, blob, off = b"", b"", base
    for i, (name, data) in enumerate(files):
        entries += name.encode().ljust(32, b"\0") + struct.pack(">QQI", off, len(data), 0) + bytes(12)
        pad = (-len(data)) % 16 if i < len(files) - 1 else 0
        blob += data + bytes(pad)
        off += len(data) + pad
    head = bytes.fromhex("dca24d00") + struct.pack(">IQII", 2, off, len(files), 64) + bytes(4)
    head = head.ljust(0x40, b"\0")
    body = head + entries + blob
    sha = hashlib.sha1(body).digest()
    return body[:0x1c] + sha + body[0x30:]


def build_set(suffix, name, keys, first_id, platinum, data, atlases, cover, art, root):
    head = ('<trophyconf version="1.1" platform="psp2" policy="large">\n <npcommid>%s_%s</npcommid>\n'
            ' <trophyset-version>01.00</trophyset-version>\n <parental-level license-area="default">0</parental-level>\n' % (TITLE, suffix))
    conf = [SIGN, head]
    trop = [SIGN, head, " <title-name>%s</title-name>\n <title-detail>%s</title-detail>\n" % (name, DETAIL)]
    pngs = [("ICON0.PNG", cover)]
    if platinum:
        conf.append(' <trophy id="000" hidden="no" ttype="P" pid="-1"/>\n')
        trop.append(' <trophy id="000" hidden="no" ttype="P" pid="-1">\n  <name>Survivor</name>\n  <detail>Earn every other trophy in this set.</detail>\n </trophy>\n')
        pngs.append(("TROP000.PNG", art))
    pid = "000" if platinum else "-1"
    for i, k in enumerate(keys, first_id):
        a = data["achievements"][k]
        g = grade(a)
        conf.append(' <trophy id="%03d" hidden="no" ttype="%s" pid="%s"/>\n' % (i, g, pid))
        trop.append(' <trophy id="%03d" hidden="no" ttype="%s" pid="%s">\n  <name>%s</name>\n  <detail>%s</detail>\n </trophy>\n'
                    % (i, g, pid, escape(title(a["description"])), escape(a["description"].replace("\n", " "))))
        pngs.append(("TROP%03d.PNG" % i, icon_png(lookup(a, data, atlases))))
    conf.append("</trophyconf>\n")
    trop.append("</trophyconf>\n")
    out = root / "sce_sys/trophy" / (TITLE + "_" + suffix) / "TROPHY.TRP"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(trp([("TROPCONF.SFM", "".join(conf).encode()), ("TROP.SFM", "".join(trop).encode())] + pngs))
    print(out.parent.name, len(keys), "trophies", out.stat().st_size, "bytes")


def main():
    gd = Path(sys.argv[1])
    root = Path(__file__).resolve().parent.parent
    data = {n: json.loads((gd / "data" / (n + ".json")).read_text(encoding="utf-8"))
            for n in ("achievements", "weapons", "characters", "items", "powerups", "arcana", "stages")}
    keys = list(data["achievements"])
    atlases = Atlases(gd / "img")
    art = Image.open(root / "sce_sys/icon0.png").convert("RGBA").resize((ICON, ICON), Image.LANCZOS)
    bg = Image.open(root / "sce_sys/livearea/contents/bg.png").convert("RGB")
    h = bg.width * 176 // 320
    cover = bg.crop((0, (bg.height - h) // 2, bg.width, (bg.height + h) // 2)).resize((320, 176), Image.LANCZOS)
    def png(im):
        b = io.BytesIO()
        im.save(b, "PNG", optimize=True)
        return b.getvalue()
    # A set holds at most 128 trophies: the platinum and 127 here, the rest in a second set without platinum.
    build_set("00", NAME, keys[:SET0], 1, True, data, atlases, png(cover), png(art), root)
    build_set("01", NAME + " (2)", keys[SET0:], 0, False, data, atlases, png(cover), png(art), root)
    (root / "src/trophy_ids.h").write_text(
        "// Generated by tools/make_trophies.py. Index < TROPHY_SET0: set 0, id = index + 1 (0 is the platinum);\n"
        "// otherwise set 1, id = index - TROPHY_SET0.\n"
        "#define TROPHY_COUNT %d\n#define TROPHY_SET0 %d\nstatic const char *const TROPHY_KEYS[TROPHY_COUNT] = {\n%s};\n"
        % (len(keys), SET0, "".join('    "%s",\n' % k for k in keys)))


if __name__ == "__main__":
    main()
