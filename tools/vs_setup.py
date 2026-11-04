#!/usr/bin/env python3
"""Vampire Survivors for PS Vita - setup tool.

Finds your own copy of Vampire Survivors (the Phaser build: Steam beta branch
"old-engine"), extracts the game data from it and sends it to your Vita, over
Wi-Fi (VitaShell's FTP server) or to a folder / the memory card over USB.
Nothing of the game is in this tool: it only reads your copy.

    python3 vs_setup.py                                   # opens the window
    python3 vs_setup.py extract "<game folder>" [out]     # extract only
    python3 vs_setup.py send "<game folder>" <vita ip>    # extract and send over FTP
"""
# Il JS del gioco gira in QuickJS (pacchetto quickjs, al posto di Node), i video li ricodifica l'ffmpeg di
# imageio-ffmpeg: chi usa l'exe non deve installare niente. Invio e destinazioni come il tool di Monster Shooter.
import ftplib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

DATA_DIR = "VampireSurvivors"   # ux0:data/VampireSurvivors
FTP_PORT = 1337
RETRIES = 3
SHACCCG = ["ur0:/data/external/libshacccg.suprx", "ur0:/data/libshacccg.suprx"]
COPY_DIRS = ("img", "tilesets", "lang", "fonts", "sfx")
VIDEOS = ("Chestnobg", "Chestnobg2", "Chestnobg3")
GAME_DIR = "Vampire Survivors"
BRANCH_HELP = ("This is the current (Unity) version of the game. The port needs the older Phaser build: "
               "in Steam, right-click Vampire Survivors > Properties > Betas, choose \"old-engine\", "
               "let it download, then try again.")


def res_dir():
    """Where extract_core.js lives: next to this file, or inside the packaged exe."""
    return Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))


# --------------------------------------------------------------------------- finding the game

def steam_roots():
    """Steam installations on this computer, including Steam inside CrossOver bottles and Flatpak."""
    home = Path.home()
    roots = []
    if sys.platform == "win32":
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
                roots.append(Path(winreg.QueryValueEx(k, "SteamPath")[0]))
        except OSError:
            pass
        roots += [Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Steam",
                  Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Steam"]
    elif sys.platform == "darwin":
        roots.append(home / "Library/Application Support/Steam")
        for b in (home / "Library/Application Support/CrossOver/Bottles").glob("*"):
            roots += [b / "drive_c/Program Files (x86)/Steam", b / "drive_c/Program Files/Steam"]
    else:
        roots += [home / ".steam/steam", home / ".local/share/Steam",
                  home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"]
    seen, out = set(), []
    for r in roots:
        try:
            key = r.resolve()
        except OSError:
            continue
        if r.is_dir() and key not in seen:
            seen.add(key)
            out.append(r)
    return out


def steam_libraries(root):
    """The Steam library folders listed in root's libraryfolders.vdf (and root itself)."""
    libs = [root]
    vdf = root / "steamapps" / "libraryfolders.vdf"
    try:
        text = vdf.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return libs
    for raw in re.findall(r'"path"\s+"([^"]+)"', text):
        p = raw.replace("\\\\", "\\")
        m = re.match(r"^([A-Za-z]):\\(.*)$", p)
        if m and sys.platform != "win32":
            # Windows path inside a Wine/CrossOver bottle: <bottle>/drive_c/...
            drive = root
            while drive.name.lower() != "drive_c" and drive != drive.parent:
                drive = drive.parent
            p = str(drive.parent / ("drive_" + m.group(1).lower()) / m.group(2).replace("\\", "/"))
        libs.append(Path(p))
    return libs


def renderer_dir(game):
    """The game's web files: resources/ on Windows and Linux, inside the .app on a native Mac install."""
    game = Path(game)
    for base in [game / "resources", *sorted(game.glob("*.app/Contents/Resources"))]:
        r = base / "app" / ".webpack" / "renderer"
        if r.is_dir():
            return r
    return game / "resources" / "app" / ".webpack" / "renderer"


def game_folder(path):
    """The install folder for a path the user typed or chose; on a Mac they may pick the .app itself."""
    p = Path(path).expanduser()
    return p.parent if p.suffix == ".app" else p


def game_kind(game):
    """'phaser' (usable), 'unity' (current version: needs the old-engine branch) or None."""
    game = Path(game)
    if (renderer_dir(game) / "main.bundle.js").is_file():
        return "phaser"
    if any(game.glob("*_Data")) or any(game.glob("*.app/Contents/Resources/Data")):
        return "unity"
    return None


def find_games():
    """[(folder, kind)] for every Vampire Survivors install found."""
    found, seen = [], set()
    for root in steam_roots():
        for lib in steam_libraries(root):
            g = lib / "steamapps" / "common" / GAME_DIR
            try:
                key = g.resolve()
            except OSError:
                continue
            if g.is_dir() and key not in seen:
                seen.add(key)
                found.append((g, game_kind(g)))
    return found


# --------------------------------------------------------------------------- extracting

def extract_tables(game, out, log=print):
    import quickjs
    r = renderer_dir(game)
    main = (r / "main.bundle.js").read_text(encoding="utf-8")
    # instead of starting the game, expose webpack's require (what tools/extract_core.js expects)
    m = re.search(r"(\w+)(?:\.O|\['O'\])\(void 0x0,\[0x[0-9a-f]+\],\(\)=>\w+\(0x[0-9a-f]+\)\)", main)
    if not m:
        raise RuntimeError("Could not find the game's start-up code in main.bundle.js: unknown build.")
    main = main[:m.start()] + "(globalThis.__req=%s,0)" % m.group(1) + main[m.end():]
    ctx = quickjs.Context()
    ctx.set_memory_limit(1 << 30)
    ctx.set_max_stack_size(8 << 20)
    ctx.eval((res_dir() / "extract_core.js").read_text(encoding="utf-8"))
    ctx.eval((r / "vendors.bundle.js").read_text(encoding="utf-8"))
    ctx.eval(main)
    res = json.loads(ctx.eval("vs_extract()"))
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    for name, text in res["files"].items():
        (out / name).write_text(text, encoding="utf-8", newline="\n")
    log("  data tables: %d files" % len(res["files"]))


DLC_TABLES = {"weapon_data": "weapons", "character_data": "characters", "stage_data": "stages", "enemy_data": "enemies",
              "secret_data": "secrets", "achievement_data": "achievements", "music_data": "music",
              "limitbreak_data": "weapons_limitbreak"}


def merge_dlc(game, out, log=print):
    """What the game's DLCLoader does at start-up: for each DLC folder, tables merged with the base entries winning, files copied."""
    game, out, dlcs = Path(game), Path(out), []
    for manifest in sorted(game.glob("*/manifest.json")):
        root, m = manifest.parent, json.loads(manifest.read_text(encoding="utf-8"))
        if not m.get("dlc_type"):
            continue
        for field, table in DLC_TABLES.items():
            path = out / "data" / (table + ".json")
            data = json.loads(path.read_text(encoding="utf-8"))
            for entry in m.get(field, []):
                add = json.loads((root / entry["path"].replace(".js", ".json")).read_text(encoding="utf-8"))
                for k, v in add.items():
                    data.setdefault(k, v)
            path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8", newline="\n")
        for d in ("img", "sfx", "tilesets"):
            if (root / "assets" / d).is_dir():
                shutil.copytree(root / "assets" / d, out / d, dirs_exist_ok=True)
        dlcs.append(m["dlc_type"])
    (out / "data" / "dlc.json").write_text(json.dumps(dlcs), encoding="utf-8", newline="\n")
    log("  DLC: %s" % (", ".join(dlcs) or "none"))


def encode_videos(game, out, log=print):
    import imageio_ffmpeg
    ff = imageio_ffmpeg.get_ffmpeg_exe()
    src = renderer_dir(game) / "assets" / "video"
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    flags = {"creationflags": 0x08000000} if sys.platform == "win32" else {}   # no console window on Windows
    for v in VIDEOS:
        # H.264 640x336, 30 fps, no audio (the original track is silent): the Vita's decoder plays these
        subprocess.run([ff, "-v", "error", "-y", "-i", str(src / (v + ".mp4")), "-an", "-vf", "scale=640:336", "-r", "30",
                        "-c:v", "libx264", "-profile:v", "baseline", "-pix_fmt", "yuv420p", "-b:v", "1200k",
                        str(out / (v + ".mp4"))], check=True, **flags)
    log("  chest videos re-encoded for the Vita")


def prepare(game, out, log=print):
    kind = game_kind(game)
    if kind == "unity":
        raise RuntimeError(BRANCH_HELP)
    if kind != "phaser":
        raise RuntimeError("This folder does not look like Vampire Survivors (no resources/app/.webpack/renderer/main.bundle.js).")
    out = Path(out)
    extract_tables(game, out / "data", log)
    assets = renderer_dir(game) / "assets"
    for d in COPY_DIRS:
        shutil.rmtree(out / d, ignore_errors=True)
        shutil.copytree(assets / d, out / d)
    log("  images, tilesets, sounds, fonts and translations copied")
    merge_dlc(game, out, log)
    encode_videos(game, out / "video", log)


# --------------------------------------------------------------------------- sending (as in the Monster Shooter tool)

class FolderSink:
    """A folder on this computer, or the Vita's memory card in USB mode."""

    def __init__(self, folder):
        folder = Path(folder)
        # VitaShell's USB mode mounts ux0: itself, with app/ and data/ at the top.
        self.on_vita = (folder / "app").is_dir() and (folder / "data").is_dir()
        self.root = folder / "data" if self.on_vita else folder

    def size(self, rel):
        p = self.root / rel
        return p.stat().st_size if p.is_file() else None

    def put(self, rel, src, size, tick):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        with open(p, "wb") as out:
            while True:
                chunk = src.read(1 << 16)
                if not chunk:
                    break
                out.write(chunk)
                tick(len(chunk))

    def reconnect(self):
        pass

    def close(self):
        pass


class FtpSink:
    """The Vita over Wi-Fi, through VitaShell's FTP server (press SELECT)."""

    def __init__(self, host, port=FTP_PORT, root="ux0:/data"):
        self.host, self.port, self.root = host, port, root
        self.ftp = None
        self.reconnect()

    def reconnect(self):
        self.close()
        self.ftp = ftplib.FTP()
        self.ftp.connect(self.host, self.port, timeout=15)
        self.ftp.login()               # VitaShell takes any login
        self.ftp.voidcmd("TYPE I")     # SIZE is only meaningful in binary mode

    def _cd(self, path, create=False):
        # one segment at a time: the server wants the device ("ux0:") on its own
        self.ftp.cwd("/")
        for seg in [s for s in path.split("/") if s]:
            try:
                self.ftp.cwd(seg)
            except ftplib.error_perm:
                if not create or seg.endswith(":"):
                    raise
                self.ftp.mkd(seg)
                self.ftp.cwd(seg)

    def _size_at(self, folder, name):
        try:
            self._cd(folder)
            return self.ftp.size(name)
        except ftplib.error_perm:
            return None

    def size(self, rel):
        folder, _, name = (self.root + "/" + rel).rpartition("/")
        return self._size_at(folder, name)

    def exists(self, path):
        folder, _, name = path.rpartition("/")
        return self._size_at(folder, name) is not None

    def put(self, rel, src, size, tick):
        folder, _, name = (self.root + "/" + rel).rpartition("/")
        self._cd(folder, create=True)
        self.ftp.storbinary("STOR " + name, src, blocksize=1 << 16, callback=lambda b: tick(len(b)))

    def close(self):
        if self.ftp:
            try:
                self.ftp.quit()
            except Exception:
                self.ftp.close()
            self.ftp = None


def plan(gamedata):
    """(opener, target relative to ux0:data/, size) for every extracted file."""
    gamedata = Path(gamedata)
    items = []
    for p in sorted(gamedata.rglob("*")):
        if p.is_file():
            rel = "%s/%s" % (DATA_DIR, p.relative_to(gamedata).as_posix())
            items.append((lambda p=p: open(p, "rb"), rel, p.stat().st_size))
    return items


def send(items, sink, log, progress):
    """Send every item, skipping files already there at full size; True when all arrived."""
    total = sum(size for _, _, size in items)
    done = 0
    for opener, rel, size in items:
        if sink.size(rel) == size:
            done += size
            progress(done, total)
            continue
        for attempt in range(1, RETRIES + 1):
            sent = [0]

            def tick(n, base=done):
                sent[0] += n
                progress(base + sent[0], total)

            try:
                with opener() as src:
                    sink.put(rel, src, size, tick)
                got = sink.size(rel)
                if got != size:
                    raise OSError("arrived as %s bytes instead of %s" % (got, size))
                break
            except (OSError, EOFError, ftplib.Error) as e:
                log("  retry %d of %d  %s  (%s)" % (attempt, RETRIES, rel, e))
                if attempt == RETRIES:
                    return False
                try:
                    sink.reconnect()
                except (OSError, ftplib.Error):
                    pass
        done += size
        progress(done, total)
    return True


def check_shacccg(sink, log):
    if not isinstance(sink, FtpSink):   # ur0: is only reachable over FTP (USB mode mounts ux0: alone)
        return
    try:
        if not any(sink.exists(p) for p in SHACCCG):
            log("  WARNING: libshacccg.suprx is missing on the Vita - extract it with ShaRKBR33D, "
                "or the game shows a black screen.")
    except (OSError, ftplib.Error):
        pass


def run(game, sink, log, progress):
    """Extract into a temporary folder, then send. Returns True when everything is on the Vita/folder."""
    with tempfile.TemporaryDirectory(prefix="vs_setup_") as tmp:
        log("Extracting from %s ..." % game)
        prepare(game, tmp, log)
        items = plan(tmp)
        log("Sending %d files (%s MB) ..." % (len(items), format(sum(s for _, _, s in items) // (1 << 20), ",")))
        check_shacccg(sink, log)
        return send(items, sink, log, progress)


# --------------------------------------------------------------------------- window

def run_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass

    msgs = queue.Queue()
    root = tk.Tk()
    root.title("Vampire Survivors - PS Vita setup")
    root.resizable(False, False)
    frm = ttk.Frame(root, padding=18)
    frm.grid(sticky="nsew")
    frm.columnconfigure(1, weight=1)
    bold = ("TkDefaultFont", 10, "bold")

    # 1. the game
    ttk.Label(frm, text="1.  Your copy of Vampire Survivors", font=bold).grid(row=0, column=0, columnspan=3, sticky="w")
    game = tk.StringVar()
    game_note = tk.StringVar()
    box = ttk.Combobox(frm, textvariable=game, width=60)
    box.grid(row=1, column=0, columnspan=2, sticky="we", padx=(18, 8), pady=(4, 0))
    ttk.Label(frm, textvariable=game_note, foreground="#555", wraplength=520, justify="left").grid(
        row=2, column=0, columnspan=3, sticky="w", padx=(18, 0))

    def note_game(*_):
        text = game.get().strip()
        kind = game_kind(game_folder(text)) if text else None
        game_note.set({"phaser": "Recognised: Phaser build (old-engine).", "unity": BRANCH_HELP}.get(
            kind, "Not found: type the path or press Choose, and pick the Vampire Survivors folder."))

    def show_game(path):
        game.set(str(path) if path else "")

    game.trace_add("write", note_game)
    games = find_games()
    box["values"] = [str(g) for g, _ in games]
    best = next((g for g, k in games if k == "phaser"), games[0][0] if games else None)
    show_game(best)

    def choose_game():
        d = filedialog.askdirectory(title="Choose the Vampire Survivors folder")
        if d:
            show_game(Path(d))

    ttk.Button(frm, text="Choose...", command=choose_game).grid(row=1, column=2, sticky="e")

    # 2. where the Vita is
    ttk.Label(frm, text="2.  Where is your Vita?", font=bold).grid(row=3, column=0, columnspan=3, sticky="w", pady=(16, 0))
    mode = tk.StringVar(value="ftp")
    ttk.Radiobutton(frm, text="Over Wi-Fi  (in VitaShell, press SELECT)", variable=mode, value="ftp").grid(
        row=4, column=0, columnspan=3, sticky="w", padx=(18, 0), pady=(4, 0))
    net = ttk.Frame(frm)
    net.grid(row=5, column=0, columnspan=3, sticky="w", padx=(40, 0))
    ttk.Label(net, text="Address").grid(row=0, column=0)
    host = tk.StringVar()
    ttk.Entry(net, textvariable=host, width=16).grid(row=0, column=1, padx=(6, 10))
    ttk.Label(net, text="Port").grid(row=0, column=2)
    port = tk.StringVar(value=str(FTP_PORT))
    ttk.Entry(net, textvariable=port, width=6).grid(row=0, column=3, padx=(6, 10))
    ttk.Label(net, text="e.g. 192.168.1.23", foreground="#777").grid(row=0, column=4)
    ttk.Radiobutton(frm, text="To a folder, or the Vita's memory card over USB", variable=mode, value="folder").grid(
        row=6, column=0, columnspan=3, sticky="w", padx=(18, 0), pady=(8, 0))
    folder = tk.StringVar()
    ttk.Label(frm, textvariable=folder, width=50).grid(row=7, column=0, columnspan=2, sticky="w", padx=(40, 0))

    def choose_folder():
        d = filedialog.askdirectory(title="Choose a folder, or the Vita's drive")
        if d:
            folder.set(d)
            mode.set("folder")

    ttk.Button(frm, text="Choose...", command=choose_folder).grid(row=7, column=2, sticky="e")

    # 3. go
    bar = ttk.Progressbar(frm, length=520, maximum=1000)
    bar.grid(row=8, column=0, columnspan=3, sticky="we", pady=(18, 6))
    logbox = tk.Text(frm, width=72, height=10, state="disabled", font=("TkFixedFont", 9))
    logbox.grid(row=9, column=0, columnspan=3, sticky="we")
    go = ttk.Button(frm, text="Prepare")
    go.grid(row=10, column=2, sticky="e", pady=(10, 0))

    def say(text):
        msgs.put(("log", text))

    def work(g, how, h, p, dest):
        sink = None
        try:
            sink = FtpSink(h, p) if how == "ftp" else FolderSink(dest)
            ok = run(g, sink, say, lambda d, t: msgs.put(("bar", d * 1000 // max(t, 1))))
            where = ("ux0:data/%s on your Vita" % DATA_DIR) if how == "ftp" or sink.on_vita else str(Path(dest) / DATA_DIR)
            msgs.put(("done", ("Done. The game data is in %s.\n\nInstall the VPK with VitaShell and play." % where) if ok
                      else "Not finished: the connection kept dropping. Press Prepare again: it only sends what is missing."))
        except (OSError, ftplib.Error) as e:
            msgs.put(("done", "Could not reach the Vita at %s:%s (%s).\n\nIn VitaShell press SELECT and type the address "
                      "it shows. The computer and the Vita must be on the same Wi-Fi." % (h, p, e) if how == "ftp" else str(e)))
        except Exception as e:
            msgs.put(("done", str(e)))
        finally:
            if sink:
                sink.close()

    def start():
        g = game_folder(game.get().strip())
        if game_kind(g) != "phaser":
            messagebox.showerror("Vampire Survivors", game_note.get())
            return
        how = mode.get()
        if how == "ftp" and not host.get().strip():
            messagebox.showerror("Vampire Survivors", "Type the Vita's address (VitaShell, SELECT).")
            return
        if how == "folder" and not folder.get():
            messagebox.showerror("Vampire Survivors", "Choose a folder first.")
            return
        go.state(["disabled"])
        threading.Thread(target=work, args=(g, how, host.get().strip(), int(port.get() or FTP_PORT), folder.get()),
                         daemon=True).start()

    go.configure(command=start)

    def pump():
        while not msgs.empty():
            kind, val = msgs.get()
            if kind == "log":
                logbox.configure(state="normal")
                logbox.insert("end", val + "\n")
                logbox.see("end")
                logbox.configure(state="disabled")
            elif kind == "bar":
                bar["value"] = val
            else:
                go.state(["!disabled"])
                messagebox.showinfo("Vampire Survivors", val)
        root.after(100, pump)

    pump()
    root.mainloop()


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        run_gui()
    elif a[0] == "extract" and len(a) >= 2:
        prepare(a[1], a[2] if len(a) > 2 else "gamedata")
    elif a[0] == "send" and len(a) >= 3:
        ok = run(a[1], FtpSink(a[2]), print, lambda d, t: None)
        sys.exit(0 if ok else 1)
    elif a[0] == "find":
        for g, k in find_games():
            print("%-8s %s" % (k, g))
    else:
        sys.exit(__doc__)
