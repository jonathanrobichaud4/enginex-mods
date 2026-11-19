"""Read what the pack needs straight out of a decrypted XDJ-RX3 firmware ISO.

The ISO (what PrimeBox's rx3dec makes from XDJ-RX3.UPD with your own key; decryption isn't
done here) holds images/rootfs.cramfs, images/gui.tar.gz and images/pdj.tar.gz. Instead of
mounting it and unpacking cramfs with fusecram in a container, this reads both formats
directly. Standard library only.

    iso = Iso(path)
    iso.read("images/gui.tar.gz")          # bytes of a file
    for e in Cramfs(iso.read("images/rootfs.cramfs")).walk(): ...
"""
import stat
import struct
import zlib

SECTOR = 2048


class FormatError(Exception):
    pass


class Iso:
    """ISO 9660 with long names from Joliet or Rock Ridge (";1" and case ignored)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.joliet = False
        self.root = None
        for n in range(16, 64):
            self.f.seek(n * SECTOR)
            vd = self.f.read(SECTOR)
            if vd[1:6] != b"CD001":
                break
            if vd[0] == 1 and self.root is None:
                self.root = self._record(vd[156:190])
            elif vd[0] == 2 and vd[88:91] in (b"%/@", b"%/C", b"%/E"):
                self.root, self.joliet = self._record(vd[156:190]), True
                break
            elif vd[0] == 255:
                break
        if self.root is None:
            raise FormatError("That file isn't a decrypted firmware ISO (no ISO 9660 signature). "
                              "The .UPD has to be decrypted first.")

    def close(self):
        self.f.close()

    @staticmethod
    def _record(rec):
        extent = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        # Rock Ridge: an NM entry in the system use area after the name holds the long name
        su = 33 + nlen + (0 if nlen % 2 else 1)
        rr, pos = b"", su
        while pos + 4 <= len(rec):
            sig, ln = rec[pos:pos + 2], rec[pos + 2]
            if ln < 4:
                break
            if sig == b"NM":
                rr += rec[pos + 5:pos + ln]
            pos += ln
        return {"extent": extent, "size": size, "dir": bool(flags & 2), "name": name, "rr": rr}

    def _list(self, d):
        self.f.seek(d["extent"] * SECTOR)
        data = self.f.read(d["size"])
        out, pos = {}, 0
        while pos < len(data):
            n = data[pos]
            if n == 0:  # records don't cross sectors: skip to the next one
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            r = self._record(data[pos:pos + n])
            pos += n
            if r["name"] in (b"\0", b"\1"):
                continue
            if r["rr"]:
                name = r["rr"].decode("utf-8", "replace")
            elif self.joliet:
                name = r["name"].decode("utf-16-be", "replace")
            else:
                name = r["name"].decode("ascii", "replace")
            name = name.split(";")[0].rstrip(".").lower()
            out[name] = r
        return out

    def find(self, path):
        cur = self.root
        for part in path.strip("/").split("/"):
            if not cur["dir"]:
                return None
            cur = self._list(cur).get(part.lower())
            if cur is None:
                return None
        return cur

    def read(self, path):
        r = self.find(path)
        if r is None or r["dir"]:
            raise FormatError(f"The ISO has no {path}. Is it the XDJ-RX3 v1.20 firmware?")
        self.f.seek(r["extent"] * SECTOR)
        return self.f.read(r["size"])


CRAMFS_MAGIC = 0x28CD3D45
PAGE = 4096


class Cramfs:
    """A cramfs image (the RX3's rootfs): walk() yields (path, mode, size, data or link target)."""

    def __init__(self, data):
        self.d = data
        for base in (0, 512):  # the superblock may sit after a 512-byte pad
            if len(data) >= base + 76 and struct.unpack_from("<I", data, base)[0] == CRAMFS_MAGIC:
                self.base = base
                break
        else:
            raise FormatError("rootfs.cramfs isn't a cramfs image the pack maker can read.")
        flags = struct.unpack_from("<I", data, self.base + 8)[0]
        if flags & 0x800:  # extended block pointers (newer kernels): not something the RX3 uses
            raise FormatError("This cramfs uses a newer layout than the XDJ-RX3's.")
        self.root = self._inode(self.base + 64)

    def _inode(self, pos):
        a, b, c = struct.unpack_from("<III", self.d, pos)
        mode, uid = a & 0xFFFF, a >> 16
        size, gid = b & 0xFFFFFF, b >> 24
        namelen, offset = (c & 0x3F) * 4, (c >> 6) * 4
        name = self.d[pos + 12:pos + 12 + namelen].rstrip(b"\0").decode("utf-8", "replace")
        return {"mode": mode, "size": size, "offset": offset, "name": name, "len": 12 + namelen}

    def _data(self, ino):
        size, off = ino["size"], ino["offset"]
        if size == 0:
            return b""
        nblocks = (size + PAGE - 1) // PAGE
        ptrs = struct.unpack_from(f"<{nblocks}I", self.d, off)
        start, out = off + 4 * nblocks, []
        for end in ptrs:
            end &= 0x3FFFFFFF
            if end > start:
                out.append(zlib.decompress(self.d[start:end]))
            else:
                out.append(b"\0" * PAGE)  # a hole
            start = end
        return b"".join(out)[:size]

    def walk(self, ino=None, prefix="", data=True):
        """With data=False, file contents aren't decompressed (for counting)."""
        ino = ino or self.root
        pos, end = ino["offset"], ino["offset"] + ino["size"]
        while pos < end:
            child = self._inode(pos)
            pos += child["len"]
            path = prefix + child["name"]
            m = child["mode"]
            if stat.S_ISDIR(m):
                yield path, m, 0, None
                yield from self.walk(child, path + "/", data)
            elif stat.S_ISREG(m):
                body = self._data(child) if data else None
                yield path, m, len(body) if body is not None else child["size"], body
            elif stat.S_ISLNK(m):
                yield path, m, 0, self._data(child).decode("utf-8", "replace") if data else None
            # device nodes, FIFOs and sockets: left out, the mod makes what the player needs
