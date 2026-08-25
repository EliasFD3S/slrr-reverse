"""Parse SLRR TREE: u32 node_count, then nodes of 3 or 7 bytes."""
from __future__ import annotations
import struct
from pathlib import Path

ROOT = Path(r"c:\Users\Elias\Desktop\re-engineering\Street Legal Racing - Redline")
EXE = ROOT / "StreetLegal_Redline.exe"

# Load flag table from PE (ushort[256] at VA 0x005f0914)
data = EXE.read_bytes()
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
opt = e_lfanew + 24
image_base = struct.unpack_from("<I", data, opt + 28)[0]
opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
sec_off = opt + opt_size
num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]


def va_to_off(va: int) -> int:
    rva = va - image_base
    for i in range(num_sections):
        o = sec_off + i * 40
        vsz, sva, rsz, rdo = struct.unpack_from("<IIII", data, o + 8)
        if sva <= rva < sva + max(vsz, rsz):
            return rdo + (rva - sva)
    raise ValueError(hex(va))


FLAGS = list(struct.unpack_from("<256H", data, va_to_off(0x005F0914)))
assert FLAGS[1] == 0x1001, hex(FLAGS[1])


def node_size(op: int) -> int:
    nibble = (FLAGS[op] >> 12) & 0xF
    return 7 if nibble in (1, 2, 4) else 3


print("flags[1]=", hex(FLAGS[1]), "node_size(1)=", node_size(1))

OPNAMES = [
    "N/A", "CAST", "INSTANCEOF", "NULL", "BOOL", "INT", "FLOAT", "CHAR",
    "STRING", "RID", "N/A2", "LOCAL_LOAD", "LOCAL_CREATE", "LOCAL_STORE",
    "LOCAL_CLEAR", "LOCAL_CLEARN", "INVOKE", "INVOKESPECIAL", "INVOKESTATIC",
    "FIELD_REF_INSTANCE", "FIELD_REF_STATIC", "FIELD_REF_QUICK", "RETURN",
    "JMP", "JMP_NE", "JMP_EQ", "JMP_EQ2", "F2I", "I2F", "I2S", "F2S",
    "PUTFIELD_INSTANCE", "PUTFIELD_STATIC", "PUTFIELD_QUICK", "NEWARRAY",
    "ARRAY_STORE", "ARRAY_INIT", "EMPTYDIMS", "ARRAY_ACCESS", "NEW", "DELETE",
    "POP", "DUP", "DUP_X1", "DUP_X2", "DUP2", "SADD", "IADD", "FADD",
]


def get_tree(blob: bytes) -> bytes:
    pos = 12
    while pos + 8 <= len(blob):
        tag, sz = blob[pos : pos + 4], struct.unpack_from("<I", blob, pos + 4)[0]
        pos += 8
        if tag == b"TREE":
            return blob[pos : pos + sz]
        pos += sz
    return b""


def parse_trees(p: bytes):
    n = struct.unpack_from("<I", p, 0)[0]
    off = 4
    trees = []
    for ti in range(n):
        if off + 4 > len(p):
            raise ValueError(f"trunc header {ti}")
        nnodes = struct.unpack_from("<I", p, off)[0]
        off += 4
        nodes = []
        start = off
        for ni in range(nnodes):
            if off >= len(p):
                raise ValueError(f"trunc tree={ti} node={ni}")
            op = p[off]
            sz = node_size(op)
            if off + sz > len(p):
                raise ValueError(f"trunc bytes tree={ti} node={ni} need {sz}")
            u16 = struct.unpack_from("<H", p, off + 1)[0]
            u32 = struct.unpack_from("<I", p, off + 3)[0] if sz == 7 else None
            name = OPNAMES[op] if op < len(OPNAMES) else f"OP_{op:02x}"
            nodes.append((op, name, u16, u32, sz))
            off += sz
        trees.append((nnodes, p[start:off], nodes))
    return trees, off, len(p)


def main() -> None:
    for rel in [
        "system/Scripts/lang/Object.class",
        "system/Scripts/lang/Math.class",
        "system/Scripts/lang/System.class",
        "system/Scripts/lang/String.class",
        "system/Scripts/io/File.class",
        "system/Scripts/lang/Integer.class",
    ]:
        p = get_tree((ROOT / rel).read_bytes())
        print("===", rel, "sec", len(p))
        try:
            trees, off, total = parse_trees(p)
            print(f"  trees={len(trees)} off={off}/{total}", "OK" if off == total else "LEFTOVER")
            for i, (nn, raw, nodes) in enumerate(trees):
                if nn == 0:
                    continue
                print(f"  [{i}] nodes={nn} bytes={len(raw)}")
                for op, name, u16, u32, sz in nodes[:16]:
                    extra = f" imm={u32}" if u32 is not None else ""
                    print(f"    {name:<20} r={u16}{extra}  ({sz}b) op={op:#x}")
                if len(nodes) > 16:
                    print("    ...")
        except Exception as e:
            print("  FAIL", e)


if __name__ == "__main__":
    main()
