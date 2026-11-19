#!/usr/bin/env python3
"""Make rekordbox-pack.tgz for the EngineX rekordbox mod, and send it to the deck.

Double-click it (or run it with no arguments) for a window. From a terminal:

    python3 make-rekordbox-pack.py /path/to/extracted [-o rekordbox-pack.tgz] [--upload DECK-IP]

Needs only Python 3 (Windows, macOS or Linux). It takes either:

  - the decrypted XDJ-RX3 v1.20 firmware ISO (what PrimeBox's rx3dec makes from XDJ-RX3.UPD
    with your own key; decrypting isn't done here). The rootfs, gui and player are read
    straight out of it, cramfs included, so nothing else needs unpacking. Or
  - a folder you extracted, laid out the way rblive4 (docs/04) or PrimeBox (docs/01) describe:

    extracted/
      XDJRX3-rootfs/              the player's userland (from rootfs.cramfs)
      XDJRX3/gui/ or XDJRX3-gui/  fontdata, imagedata, pset, system
      rbp-audio or stock-rbp      the player (else XDJRX3/pdj/rbp or the rootfs' root/pdj/rbp);
                                  stock or rblive4-patched, it's patched here either way
    A folder with only XDJRX3.iso in it (PrimeBox's extracted/ after rx3dec) works too.

The pack holds only those files, with the player patched by rblive4's patch tables (pc/rblive4,
MIT). Nothing from Pioneer comes with the mod or this script, and the pack is for your own deck.
Upload it to the deck from here, copy it to a USB drive, or upload it at http://<deck-ip>:8093/.
"""
import argparse
import hashlib
import http.client
import importlib.util
import io
import json
import os
import stat
import struct
import sys
import tarfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rx3iso  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FORMAT = 1
DECK_PORT = 8093
SETTINGS = os.path.join(os.path.expanduser("~"), ".rekordbox-pack.json")


class PackError(Exception):
    """Something the person can fix: the message says what."""


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, "rblive4", file))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def patch_player(data):
    """Stock or rbp-audio in, the SC LIVE 4 player out (rblive4's two patch sets)."""
    base = load("rbp_patch", "rbp_patch.py")
    live4 = load("patch_rbp_sclive4", "patch_rbp_sclive4.py")
    sets = [((va - base.LOAD_BIAS), old, new, note) for va, old, new, note in base.PATCHES]
    sets += list(live4.PATCHES)
    applied = 0
    for off, old, new, note in sets:
        if off < 0 or off + 4 > len(data):
            raise PackError("The player file isn't the XDJ-RX3 v1.20 player (it's too short).")
        cur = struct.unpack_from("<I", data, off)[0]
        if cur == new:
            continue
        if cur != old:
            raise PackError("The player file isn't the XDJ-RX3 v1.20 player, or another tool changed it "
                            f"(at 0x{off:x}: {note}).")
        struct.pack_into("<I", data, off, new)
        applied += 1
    return applied, len(sets)


def find(base, *names):
    for n in names:
        p = os.path.join(base, *n.split("/"))
        if os.path.exists(p):
            return p
    return None


GUI_DIRS = ("fontdata", "imagedata", "pset", "system")
ISO_PARTS = ("images/rootfs.cramfs", "images/gui.tar.gz", "images/pdj.tar.gz")


def check_source(x):
    """What to make the pack from: {"iso": path} or {"rootfs", "gui", "player"} folders/files.
    PackError says what's missing."""
    if not x or not os.path.exists(x):
        raise PackError("Choose the decrypted firmware ISO, or the folder with your extracted XDJ-RX3 files.")
    if os.path.isfile(x):
        return check_iso(x)
    rootfs = find(x, "XDJRX3-rootfs", "rootfs")
    if not rootfs:
        isos = [f for f in sorted(os.listdir(x)) if f.lower().endswith(".iso")]
        if isos:
            return check_iso(os.path.join(x, isos[0]))
        raise PackError("No XDJRX3-rootfs folder (or decrypted .iso) in it. rblive4's docs/04 shows the layout.")
    gui = find(x, "XDJRX3/gui", "XDJRX3-gui", "gui")
    if not gui or not os.path.isdir(gui):
        raise PackError("No XDJRX3/gui (or XDJRX3-gui) folder in it.")
    missing = [d for d in GUI_DIRS if not os.path.isdir(os.path.join(gui, d))]
    if missing:
        raise PackError(f"The gui folder is missing: {', '.join(missing)}")
    if not find(rootfs, "lib/ld-linux.so.3"):
        raise PackError("XDJRX3-rootfs has no lib/ld-linux.so.3. Is it the whole player rootfs?")
    player = (find(x, "rbp-audio", "stock-rbp", "rbp", "XDJRX3/pdj/rbp")
              or find(rootfs, "root/pdj/rbp", "pdj/rbp"))
    if not player:
        raise PackError("Can't find the player: put rbp-audio or stock-rbp in the folder "
                        "(it's pdj/rbp inside the firmware's pdj.tar.gz).")
    return {"rootfs": rootfs, "gui": gui, "player": player}


def check_iso(path):
    if path.lower().endswith(".upd"):
        raise PackError("That's the encrypted .UPD. Decrypt it first (PrimeBox's rx3dec does that with "
                        "your own key), then choose the .iso it makes.")
    try:
        iso = rx3iso.Iso(path)
    except rx3iso.FormatError as e:
        raise PackError(str(e))
    except OSError as e:
        raise PackError(f"Can't open it: {e.strerror}")
    try:
        missing = [p for p in ISO_PARTS if iso.find(p) is None]
    finally:
        iso.close()
    if missing:
        raise PackError(f"The ISO has no {', '.join(missing)}. Is it the XDJ-RX3 v1.20 firmware?")
    return {"iso": path}


EXEC_DIRS = ("bin/", "sbin/", "usr/bin/", "usr/sbin/")


class Packer:
    def __init__(self, log=print, progress=None):
        self.log = log
        self.progress = progress or (lambda done, total: None)
        self.done = self.total = 0

    def count(self, src):
        return sum(len(files) + len(dirs) for _, dirs, files in os.walk(src))

    def add_tree(self, tar, src, dest, skip=()):
        """Add a folder as root-owned files. Execute bits get set where rblive4 sets them, since a
        Windows extraction loses them."""
        for top, dirs, files in os.walk(src):
            dirs.sort()
            rel_top = os.path.relpath(top, src).replace(os.sep, "/")
            rel_top = "" if rel_top == "." else rel_top + "/"
            for d in dirs:
                p = os.path.join(top, d)
                name = dest + rel_top + d
                if os.path.islink(p):
                    self.add_one(tar, p, name)
                else:
                    ti = tarfile.TarInfo(name)
                    ti.type, ti.mode, ti.mtime = tarfile.DIRTYPE, 0o755, int(os.path.getmtime(p))
                    tar.addfile(ti)
                self.step()
            for f in sorted(files):
                rel = rel_top + f
                if rel not in skip:
                    self.add_one(tar, os.path.join(top, f), dest + rel)
                self.step()

    def step(self):
        self.done += 1
        if self.done % 50 == 0 or self.done == self.total:
            self.progress(self.done, self.total)

    def add_one(self, tar, path, name):
        try:
            ti = tar.gettarinfo(path, name)
        except OSError as e:
            self.log(f"  skipped {name}: {e}")
            return
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = "root"
        if ti.isreg():
            inner = name.split("/", 1)[1] if name.startswith("./") else name
            if inner.startswith(EXEC_DIRS) or "/ld-" in "/" + inner or inner.endswith("ld-linux.so.3"):
                ti.mode = 0o755
            else:
                ti.mode = (ti.mode & 0o777) or 0o644
            try:
                with open(path, "rb") as fh:
                    tar.addfile(ti, fh)
            except OSError as e:
                self.log(f"  skipped {name}: {e}")
        elif ti.issym() or ti.isdir():
            tar.addfile(ti)
        # devices, FIFOs and sockets are left out: the mod makes what the player needs

    def make(self, x, out):
        src = check_source(x)
        tmp = out + ".part"
        try:
            with tarfile.open(tmp, "w:gz", compresslevel=6, format=tarfile.GNU_FORMAT) as tar:
                if "iso" in src:
                    rbp, name = self.from_iso(tar, src["iso"])
                else:
                    rbp, name = self.from_folder(tar, src, x)
                applied, total = patch_player(rbp)
                md5 = hashlib.md5(rbp).hexdigest()
                self.log(f"Player: {name} ({applied} of {total} patches applied here)")
                add_bytes(tar, "./root/pdj/rbp", bytes(rbp), 0o755)
                stamp = time.strftime("%Y-%m-%d")
                add_bytes(tar, "./.rekordbox-pack",
                          f"XDJ-RX3 player (md5 {md5[:8]}), pack made {stamp}\nformat {FORMAT}\n".encode())
            os.replace(tmp, out)
        except BaseException:
            try:
                os.remove(tmp)
            except OSError:
                pass
            raise
        self.progress(self.total, self.total)
        self.log(f"Done: {out} ({os.path.getsize(out) / 1e6:.0f} MB)")
        return out

    def from_folder(self, tar, src, x):
        with open(src["player"], "rb") as fh:
            rbp = bytearray(fh.read())
        patch_player(bytearray(rbp))  # a wrong player shows up now, not after a minute of packing
        gui_dirs = [os.path.join(src["gui"], d) for d in GUI_DIRS]
        self.done, self.total = 0, self.count(src["rootfs"]) + sum(self.count(d) for d in gui_dirs)
        self.log(f"Writing the pack from {x} (this takes a minute)...")
        self.add_tree(tar, src["rootfs"], "./", skip={"root/pdj/rbp", "etc/mtab"})
        for d, path in zip(GUI_DIRS, gui_dirs):
            self.add_tree(tar, path, f"./root/gui/{d}/")
        return rbp, os.path.relpath(src["player"], x)

    def from_iso(self, tar, path):
        iso = rx3iso.Iso(path)
        try:
            self.log(f"Reading {os.path.basename(path)}...")
            pdj = tarfile.open(fileobj=io.BytesIO(iso.read("images/pdj.tar.gz")), mode="r:gz")
            member = next((m for m in pdj.getmembers()
                           if m.isfile() and m.name.lstrip("./").split("/")[-1] == "rbp"), None)
            if member is None:
                raise PackError("The firmware's pdj.tar.gz has no rbp in it.")
            rbp = bytearray(pdj.extractfile(member).read())
            patch_player(bytearray(rbp))
            gui = tarfile.open(fileobj=io.BytesIO(iso.read("images/gui.tar.gz")), mode="r:gz")
            gui_members = [m for m in gui.getmembers() if m.name.strip("./")]
            try:
                fs = rx3iso.Cramfs(iso.read("images/rootfs.cramfs"))
            except rx3iso.FormatError as e:
                raise PackError(str(e))
        finally:
            iso.close()
        self.done = 0
        self.total = sum(1 for _ in fs.walk(data=False)) + len(gui_members)
        self.log("Writing the pack (this takes a minute)...")
        for rel, mode, size, body in fs.walk():
            if rel in ("root/pdj/rbp", "etc/mtab"):
                self.step()
                continue
            name = "./" + rel
            ti = tarfile.TarInfo(name)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            ti.mtime = int(time.time())
            if stat.S_ISDIR(mode):
                ti.type, ti.mode = tarfile.DIRTYPE, 0o755
                tar.addfile(ti)
            elif stat.S_ISLNK(mode):
                ti.type, ti.linkname, ti.mode = tarfile.SYMTYPE, body, 0o777
                tar.addfile(ti)
            else:
                ti.size, ti.mode = size, (mode & 0o7777) or 0o644
                tar.addfile(ti, io.BytesIO(body))
            self.step()
        for m in gui_members:
            m.name = "./root/gui/" + m.name.lstrip("./")
            m.uid = m.gid = 0
            m.uname = m.gname = "root"
            if m.isfile():
                tar.addfile(m, gui.extractfile(m))
            elif m.isdir() or m.issym():
                tar.addfile(m)
            self.step()
        return rbp, "pdj/rbp from the ISO"


def add_bytes(tar, name, data, mode=0o644):
    ti = tarfile.TarInfo(name)
    ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
    tar.addfile(ti, io.BytesIO(data))


# ---------------------------------------------------------------------------------- to the deck

def deck_host(text):
    """'192.168.1.20', 'http://192.168.1.20:8093/' or 'sclive4.local' -> (host, port)."""
    t = text.strip()
    for p in ("http://", "https://"):
        if t.startswith(p):
            t = t[len(p):]
    t = t.split("/")[0]
    if not t:
        raise PackError("Type the deck's address (it's in Settings > ABOUT / UPDATE, after Web Console).")
    host, _, port = t.partition(":")
    return host, int(port) if port.isdigit() else DECK_PORT


def deck_status(host, port, timeout=5):
    c = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        c.request("GET", "/status")
        return json.loads(c.getresponse().read() or b"{}")
    finally:
        c.close()


def upload(path, address, log=print, progress=None):
    """Send the pack to the deck's rekordbox launcher and wait while the deck installs it."""
    progress = progress or (lambda done, total: None)
    host, port = deck_host(address)
    try:
        st = deck_status(host, port)
    except (OSError, ValueError):
        raise PackError(f"No answer from the rekordbox mod at {host}:{port}. Is the deck on, on this "
                        "network, with the rekordbox mod installed?")
    if st.get("state") in ("starting", "running", "stopping"):
        raise PackError("rekordbox is running on the deck. Go back to Engine first (SHIFT + BACK).")
    size = os.path.getsize(path)
    log(f"Sending {os.path.basename(path)} to {host} ({size / 1e6:.0f} MB)...")
    c = http.client.HTTPConnection(host, port, timeout=60)
    try:
        c.putrequest("POST", "/upload")
        c.putheader("Content-Type", "application/octet-stream")
        c.putheader("Content-Length", str(size))
        c.endheaders()
        sent = 0
        with open(path, "rb") as fh:
            while True:
                chunk = fh.read(256 * 1024)
                if not chunk:
                    break
                c.send(chunk)
                sent += len(chunk)
                progress(sent, size)
        r = c.getresponse()
        body = json.loads(r.read() or b"{}")
    except OSError as e:
        raise PackError(f"The upload stopped: {e}")
    finally:
        c.close()
    if body.get("error"):
        raise PackError(f"The deck said: {body['error']}")
    log("Sent. The deck is installing it...")
    for _ in range(600):
        time.sleep(1)
        try:
            st = deck_status(host, port)
        except (OSError, ValueError):
            continue
        if st.get("state") == "ready":
            log("The deck has it. Press Start under rekordbox in Settings > ABOUT / UPDATE.")
            return
        if st.get("state") == "error":
            raise PackError(f"The deck couldn't install it: {st.get('message', '')}")
    raise PackError("The deck is taking very long to install it. Check http://%s:%d/" % (host, port))


# ---------------------------------------------------------------------------------- the window

def gui():
    import queue
    import tkinter as tk
    from tkinter import filedialog, ttk

    try:
        saved = json.load(open(SETTINGS))
    except (OSError, ValueError):
        saved = {}

    root = tk.Tk()
    root.title("rekordbox pack for the SC LIVE 4")
    root.minsize(560, 420)
    pad = {"padx": 10, "pady": 4}
    events = queue.Queue()
    busy = tk.BooleanVar(value=False)

    folder = tk.StringVar(value=saved.get("folder", ""))
    out = tk.StringVar(value=saved.get("output") or os.path.join(os.path.expanduser("~"), "rekordbox-pack.tgz"))
    deck = tk.StringVar(value=saved.get("deck", ""))
    check = tk.StringVar()

    ttk.Style(root).configure("Step.TLabel", font=("TkDefaultFont", 11, "bold"))
    frm = ttk.Frame(root, padding=8)
    frm.pack(fill="both", expand=True)
    frm.columnconfigure(1, weight=1)

    ttk.Label(frm, text="1. Your XDJ-RX3 v1.20 firmware: the decrypted .iso, or a folder you extracted", style="Step.TLabel").grid(row=0, column=0, columnspan=3, sticky="w", **pad)
    ttk.Entry(frm, textvariable=folder).grid(row=1, column=0, columnspan=2, sticky="ew", **pad)
    picks = ttk.Frame(frm)
    picks.grid(row=1, column=2, **pad)
    ttk.Button(picks, text="ISO...", width=7, command=lambda: pick_iso()).pack(side="left")
    ttk.Button(picks, text="Folder...", width=8, command=lambda: pick_folder()).pack(side="left", padx=(4, 0))
    check_lbl = ttk.Label(frm, textvariable=check, wraplength=520)
    check_lbl.grid(row=2, column=0, columnspan=3, sticky="w", padx=10)

    ttk.Label(frm, text="2. Make the pack", style="Step.TLabel").grid(row=3, column=0, columnspan=3, sticky="w", **pad)
    ttk.Entry(frm, textvariable=out).grid(row=4, column=0, columnspan=2, sticky="ew", **pad)
    ttk.Button(frm, text="Save as...", command=lambda: pick_out()).grid(row=4, column=2, **pad)
    make_btn = ttk.Button(frm, text="Make pack", command=lambda: start_make())
    make_btn.grid(row=5, column=0, sticky="w", **pad)

    ttk.Label(frm, text="3. Send it to the deck (or copy it to a USB drive and press Set up on the deck)", style="Step.TLabel").grid(
        row=6, column=0, columnspan=3, sticky="w", **pad)
    ttk.Label(frm, text="Deck address:").grid(row=7, column=0, sticky="w", **pad)
    ttk.Entry(frm, textvariable=deck).grid(row=7, column=1, sticky="ew", **pad)
    send_btn = ttk.Button(frm, text="Send to deck", command=lambda: start_send())
    send_btn.grid(row=7, column=2, **pad)
    ttk.Label(frm, text="The address is in Settings > ABOUT / UPDATE on the deck, after Web Console "
                        "(just the numbers, like 192.168.1.20).", foreground="#777", wraplength=520).grid(
        row=8, column=0, columnspan=3, sticky="w", padx=10)

    bar = ttk.Progressbar(frm, maximum=100)
    bar.grid(row=9, column=0, columnspan=3, sticky="ew", padx=10, pady=(12, 4))
    log_box = tk.Text(frm, height=8, wrap="word", state="disabled", relief="flat", background=root.cget("background"))
    log_box.grid(row=10, column=0, columnspan=3, sticky="nsew", padx=10, pady=4)
    frm.rowconfigure(10, weight=1)

    def save():
        try:
            json.dump({"folder": folder.get(), "output": out.get(), "deck": deck.get()}, open(SETTINGS, "w"))
        except OSError:
            pass

    def refresh(*_):
        try:
            parts = check_source(folder.get())
            if "iso" in parts:
                check.set(f"Looks right: {os.path.basename(parts['iso'])} has the rootfs, gui and player.")
            else:
                player = os.path.basename(parts["player"])
                check.set(f"Looks right: rootfs, gui and the player ({player}) are there.")
            check_lbl.configure(foreground="#1a7f37")
        except PackError as e:
            check.set(str(e) if folder.get() else "")
            check_lbl.configure(foreground="#b42318")
        idle = not busy.get()
        make_btn.state(["!disabled"] if idle and check.get().startswith("Looks") else ["disabled"])
        send_btn.state(["!disabled"] if idle and os.path.isfile(out.get()) and deck.get().strip() else ["disabled"])

    for v in (folder, out, deck, busy):
        v.trace_add("write", refresh)

    def pick_folder():
        d = filedialog.askdirectory(title="Folder with XDJRX3-rootfs and XDJRX3/gui", initialdir=folder.get() or None)
        if d:
            folder.set(d)

    def pick_iso():
        f = filedialog.askopenfilename(title="The decrypted firmware ISO",
                                       filetypes=[("Firmware ISO", "*.iso"), ("All files", "*")])
        if f:
            folder.set(f)

    def pick_out():
        f = filedialog.asksaveasfilename(title="Save the pack as", defaultextension=".tgz",
                                         initialfile="rekordbox-pack.tgz", filetypes=[("rekordbox pack", "*.tgz")])
        if f:
            out.set(f)

    def log(msg):
        events.put(("log", msg))

    def progress(done, total):
        events.put(("progress", 100.0 * done / total if total else 0))

    def work(fn):
        def run():
            try:
                fn()
            except PackError as e:
                log(str(e))
            except Exception as e:  # anything else: say it rather than vanish
                log(f"Something went wrong: {e!r}")
            events.put(("done", None))
        save()
        busy.set(True)
        bar["value"] = 0
        threading.Thread(target=run, daemon=True).start()

    def start_make():
        work(lambda: Packer(log, progress).make(folder.get(), out.get()))

    def start_send():
        work(lambda: upload(out.get(), deck.get(), log, progress))

    def pump():
        try:
            while True:
                kind, val = events.get_nowait()
                if kind == "log":
                    log_box.configure(state="normal")
                    log_box.insert("end", val + "\n")
                    log_box.see("end")
                    log_box.configure(state="disabled")
                elif kind == "progress":
                    bar["value"] = val
                elif kind == "done":
                    busy.set(False)
        except queue.Empty:
            pass
        root.after(100, pump)

    refresh()
    pump()
    root.mainloop()


def main():
    if len(sys.argv) == 1:
        try:
            return gui()
        except ImportError:
            print("This Python has no Tkinter, so no window. Run it with the folder instead:\n"
                  "  python3 make-rekordbox-pack.py /path/to/extracted")
            return 1
    ap = argparse.ArgumentParser(description="Make rekordbox-pack.tgz from your extracted XDJ-RX3 files.")
    ap.add_argument("extracted", help="the decrypted firmware .iso, or a folder with XDJRX3-rootfs/ and the gui")
    ap.add_argument("-o", "--output", default="rekordbox-pack.tgz")
    ap.add_argument("--upload", metavar="DECK-IP", help="send the pack to the deck afterwards")
    a = ap.parse_args()
    try:
        out = Packer().make(a.extracted, a.output)
        if a.upload:
            upload(out, a.upload)
        else:
            print("Next: put it on a USB drive and press Set up in Settings > ABOUT / UPDATE on the deck,")
            print("or send it with --upload DECK-IP, or upload it at http://<deck-ip>:8093/")
    except PackError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
