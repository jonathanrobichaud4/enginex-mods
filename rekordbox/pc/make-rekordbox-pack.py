#!/usr/bin/env python3
"""Make rekordbox-pack.tgz for the EngineX rekordbox mod.

    python3 make-rekordbox-pack.py /path/to/extracted [-o rekordbox-pack.tgz]

Needs only Python 3 (Windows, macOS or Linux). The folder is your own extracted XDJ-RX3 v1.20
files, laid out the way rblive4 describes (https://github.com/Malphite31/rblive4, docs/04):

    extracted/
      XDJRX3-rootfs/     the player's userland (from rootfs.cramfs)
      XDJRX3/gui/        fontdata, imagedata, pset, system
      rbp-audio          optional: rblive4's patched player. Without it, the stock player is
                         taken from XDJRX3-rootfs/root/pdj/rbp (or extracted/rbp) and patched here.

The pack holds only those files, with the player patched by rblive4's patch tables (pc/rblive4,
MIT). Nothing from Pioneer comes with the mod or this script, and the pack is for your own deck.
Copy it to a USB drive, or upload it at http://<deck-ip>:8093/.
"""
import argparse
import hashlib
import importlib.util
import io
import os
import struct
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FORMAT = 1


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
            raise SystemExit("This isn't the XDJ-RX3 v1.20 player (it's too short).")
        cur = struct.unpack_from("<I", data, off)[0]
        if cur == new:
            continue
        if cur != old:
            raise SystemExit(f"This isn't the XDJ-RX3 v1.20 player, or it was changed by another tool "
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


EXEC_DIRS = ("bin/", "sbin/", "usr/bin/", "usr/sbin/")


def add_tree(tar, src, dest, skip=()):
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
                add_one(tar, p, name)
            else:
                ti = tarfile.TarInfo(name)
                ti.type, ti.mode, ti.mtime = tarfile.DIRTYPE, 0o755, int(os.path.getmtime(p))
                tar.addfile(ti)
        for f in sorted(files):
            rel = rel_top + f
            if rel in skip:
                continue
            add_one(tar, os.path.join(top, f), dest + rel)


def add_one(tar, path, name):
    try:
        ti = tar.gettarinfo(path, name)
    except OSError as e:
        print(f"  skipped {name}: {e}")
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
            print(f"  skipped {name}: {e}")
    elif ti.issym() or ti.isdir():
        tar.addfile(ti)
    # devices, FIFOs and sockets are left out: the mod makes what the player needs


def add_bytes(tar, name, data, mode=0o644):
    ti = tarfile.TarInfo(name)
    ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
    tar.addfile(ti, io.BytesIO(data))


def main():
    ap = argparse.ArgumentParser(description="Make rekordbox-pack.tgz from your extracted XDJ-RX3 files.")
    ap.add_argument("extracted", help="folder with XDJRX3-rootfs/ and XDJRX3/gui/ (see rblive4 docs/04)")
    ap.add_argument("-o", "--output", default="rekordbox-pack.tgz")
    a = ap.parse_args()

    x = a.extracted
    rootfs = find(x, "XDJRX3-rootfs", "rootfs")
    gui = find(x, "XDJRX3/gui", "gui")
    if not rootfs or not os.path.isdir(rootfs):
        raise SystemExit(f"No XDJRX3-rootfs folder in {x}. See rblive4's docs/04-firmware-assets.md for the layout.")
    if not gui or not os.path.isdir(gui):
        raise SystemExit(f"No XDJRX3/gui folder in {x}.")
    missing = [d for d in ("fontdata", "imagedata", "pset", "system") if not os.path.isdir(os.path.join(gui, d))]
    if missing:
        raise SystemExit(f"{gui} is missing: {', '.join(missing)}")
    if not find(rootfs, "lib/ld-linux.so.3"):
        raise SystemExit(f"{rootfs} has no lib/ld-linux.so.3; is it the whole player rootfs?")
    player = find(x, "rbp-audio", "rbp") or find(rootfs, "root/pdj/rbp", "pdj/rbp")
    if not player:
        raise SystemExit("Can't find the player: put rbp-audio in the folder, or keep root/pdj/rbp in the rootfs.")

    with open(player, "rb") as fh:
        rbp = bytearray(fh.read())
    applied, total = patch_player(rbp)
    md5 = hashlib.md5(rbp).hexdigest()
    print(f"player: {os.path.relpath(player, x)}, {applied} of {total} patches applied here, md5 {md5}")

    out = a.output
    tmp = out + ".part"
    stamp = time.strftime("%Y-%m-%d")
    print(f"writing {out} (this takes a minute)...")
    with tarfile.open(tmp, "w:gz", compresslevel=6, format=tarfile.GNU_FORMAT) as tar:
        add_tree(tar, rootfs, "./", skip={"root/pdj/rbp", "etc/mtab"})
        for d in ("fontdata", "imagedata", "pset", "system"):
            add_tree(tar, os.path.join(gui, d), f"./root/gui/{d}/")
        add_bytes(tar, "./root/pdj/rbp", bytes(rbp), 0o755)
        add_bytes(tar, "./.rekordbox-pack", f"XDJ-RX3 player (md5 {md5[:8]}), pack made {stamp}\nformat {FORMAT}\n".encode())
    os.replace(tmp, out)
    print(f"done: {out}, {os.path.getsize(out) / 1e6:.0f} MB")
    print("Next: put it on a USB drive and press Set up in Settings > ABOUT / UPDATE on the deck,")
    print("or upload it at http://<deck-ip>:8093/")


if __name__ == "__main__":
    sys.exit(main())
