#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""snapsafe: the .rfm1 modules whose working memory a snapshot can capture whole (README "The working memory").

A firmware's state between two calls is its wasm instance: the linear memory, the globals, the tables and the dropped
segments (wasm2c 1.0.32's instance struct: one pointer per imported module, then the module's own globals in order,
then the memory, then the tables). The host snapshots the memory and the globals (it finds them in the instance at the
offset this table gives, and checks the memory's own data pointer right after them), so it can restore exactly any
module whose tables and segments never change after start-up. This reads each .rfm1 (a wasm module) and says "safe"
when:
  - no instruction changes a table (table.set / grow / fill / copy / init, elem.drop), nor a passive data segment
    (memory.init, data.drop), and none uses exception handling (wasm2c keeps nothing of it in the instance, but a
    module that throws is not one these checks were made for);
  - it imports no global, memory or table (its globals are all its own, laid out as the table says);
  - it is not a Fantasy CFW (rfm1_host_1 imports: the file service's open handles live in the host).
memory.grow is allowed (the host compares the memory's size at a restore and refuses another).

  tools/snapsafe.py FILE.rfm1 ..            the verdict per file
  tools/snapsafe.py --header OUT.h FILE..   src/snapsafe.h: the safe modules' SHA-256 with their globals' place
"""
import hashlib
import struct
import sys


class Bad(Exception):
    pass


class R:
    def __init__(self, b, i=0, end=None):
        self.b, self.i, self.end = b, i, len(b) if end is None else end

    def u8(self):
        if self.i >= self.end:
            raise Bad("eof")
        v = self.b[self.i]
        self.i += 1
        return v

    def uleb(self):
        r = s = 0
        while True:
            x = self.u8()
            r |= (x & 0x7F) << s
            s += 7
            if x < 0x80:
                return r

    def sleb(self):
        r = s = 0
        while True:
            x = self.u8()
            r |= (x & 0x7F) << s
            s += 7
            if x < 0x80:
                if x & 0x40:
                    r -= 1 << s
                return r

    def skip(self, n):
        self.i += n

    def name(self):
        n = self.uleb()
        s = self.b[self.i:self.i + n].decode("utf-8", "replace")
        self.i += n
        return s


def blocktype(r):
    x = r.b[r.i]
    if x == 0x40 or x in (0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x70, 0x6F):
        r.i += 1
    else:
        r.sleb()   # a type index (s33)


def memarg(r):
    a = r.uleb()
    if a & 0x40:   # multi-memory: an index
        r.uleb()
    r.uleb()


def scan_code(r, end, hit):
    """the instructions of one body to end; hit(op, imm) for the ones that matter"""
    while r.i < end:
        op = r.u8()
        if op in (0x02, 0x03, 0x04):
            blocktype(r)
        elif op in (0x0C, 0x0D):
            r.uleb()
        elif op == 0x0E:
            for _ in range(r.uleb() + 1):
                r.uleb()
        elif op == 0x10:
            hit("call", r.uleb())
        elif op == 0x11:
            r.uleb(); r.uleb()
        elif op in (0x12,):
            hit("call", r.uleb())   # return_call
        elif op == 0x13:
            r.uleb(); r.uleb()
        elif op in (0x06, 0x07, 0x08, 0x09, 0x18, 0x19, 0x1F, 0x0A):
            hit("exceptions", op)
            if op in (0x06,):
                blocktype(r)
            elif op in (0x07, 0x08, 0x09, 0x18):
                r.uleb()
            elif op == 0x1F:
                blocktype(r)
                for _ in range(r.uleb()):
                    k = r.u8()
                    if k in (0, 1):
                        r.uleb()
                    r.uleb()
        elif op == 0x1C:
            for _ in range(r.uleb()):
                r.u8()
        elif op in (0x20, 0x21, 0x22):
            r.uleb()
        elif op == 0x23:
            hit("global.get", r.uleb())
        elif op == 0x24:
            hit("global.set", r.uleb())
        elif op in (0x25, 0x26):
            t = r.uleb()
            if op == 0x26:
                hit("table", "table.set")
        elif 0x28 <= op <= 0x3E:
            memarg(r)
        elif op in (0x3F, 0x40):
            r.uleb()
            if op == 0x40:
                hit("memory.grow", 0)
        elif op == 0x41:
            hit("i32.const", r.sleb())
        elif op == 0x42:
            r.sleb()
        elif op == 0x43:
            r.skip(4)
        elif op == 0x44:
            r.skip(8)
        elif op == 0xD0:
            r.u8()
        elif op == 0xD2:
            r.uleb()
        elif op == 0xFC:
            sub = r.uleb()
            if sub <= 7:
                pass
            elif sub == 8:
                r.uleb(); r.uleb(); hit("passive", "memory.init")
            elif sub == 9:
                r.uleb(); hit("passive", "data.drop")
            elif sub == 10:
                r.uleb(); r.uleb()
            elif sub == 11:
                r.uleb()
            elif sub == 12:
                r.uleb(); r.uleb(); hit("table", "table.init")
            elif sub == 13:
                r.uleb(); hit("table", "elem.drop")
            elif sub == 14:
                r.uleb(); r.uleb(); hit("table", "table.copy")
            elif sub == 15:
                r.uleb(); hit("table", "table.grow")
            elif sub == 16:
                r.uleb()
            elif sub == 17:
                r.uleb(); hit("table", "table.fill")
            else:
                raise Bad("0xFC %d" % sub)
        elif op == 0xFD:
            sub = r.uleb()
            if sub <= 11 or sub in (92, 93):
                memarg(r)
            elif sub == 12 or sub == 13:
                r.skip(16)
            elif 21 <= sub <= 34:
                r.u8()
            elif 84 <= sub <= 91:
                memarg(r); r.u8()
            else:
                pass
        elif op == 0xFE:
            sub = r.uleb()
            if sub == 3:
                r.u8()
            else:
                memarg(r)
        elif op == 0x0B or op == 0x05 or op == 0x00 or op == 0x01 or op == 0x0F or op == 0x1A or op == 0x1B \
                or 0x45 <= op <= 0xC4 or op == 0xD1 or op == 0xD3 or op == 0xD4:
            pass
        else:
            raise Bad("opcode 0x%02x at %d" % (op, r.i - 1))


def analyse(b):
    if b[:8] != b"\0asm\x01\0\0\0":
        raise Bad("not wasm")
    r = R(b, 8)
    nfimp = ngimp = 0
    imports, exports, globals_, start = [], {}, [], None
    bodies = []
    ntypes = 0
    host1 = False
    mem = None
    func_names, global_names = {}, {}
    while r.i < len(b):
        sid = r.u8()
        size = r.uleb()
        end = r.i + size
        s = R(b, r.i, end)
        if sid == 2:
            for _ in range(s.uleb()):
                m, n = s.name(), s.name()
                k = s.u8()
                if k == 0:
                    s.uleb(); nfimp += 1
                elif k == 1:
                    s.u8(); fl = s.uleb(); s.uleb()
                    if fl & 1:
                        s.uleb()
                elif k == 2:
                    fl = s.uleb(); s.uleb()
                    if fl & 1:
                        s.uleb()
                elif k == 3:
                    s.u8(); s.u8(); ngimp += 1
                else:
                    raise Bad("import kind %d" % k)
                imports.append((m, n, k))
                if m == "rfm1_host_1":
                    host1 = True
        elif sid == 5:
            for _ in range(s.uleb()):
                fl = s.uleb(); lo = s.uleb(); hi = s.uleb() if fl & 1 else None
                mem = (lo, hi, bool(fl & 2))
        elif sid == 6:
            for k in range(s.uleb()):
                t = s.u8(); mut = s.u8()
                # init expr
                init = None
                while True:
                    op = s.u8()
                    if op == 0x0B:
                        break
                    if op == 0x41:
                        init = s.sleb()
                    elif op == 0x42:
                        init = s.sleb()
                    elif op == 0x43:
                        s.skip(4)
                    elif op == 0x44:
                        s.skip(8)
                    elif op == 0x23:
                        s.uleb()
                    elif op == 0xD0:
                        s.u8()
                    elif op == 0xD2:
                        s.uleb()
                    elif op in (0x6A, 0x6B, 0x6C, 0x7C, 0x7D, 0x7E):
                        pass
                    else:
                        raise Bad("global init 0x%02x" % op)
                globals_.append((t, mut, init))
        elif sid == 7:
            for _ in range(s.uleb()):
                n = s.name(); k = s.u8(); idx = s.uleb()
                exports[n] = (k, idx)
        elif sid == 8:
            start = s.uleb()
        elif sid == 10:
            for k in range(s.uleb()):
                sz = s.uleb()
                be = s.i + sz
                for _ in range(s.uleb()):
                    s.uleb(); s.u8()
                bodies.append((s.i, be))
                s.i = be
        elif sid == 0:
            n = s.name()
            if n == "name":
                while s.i < end:
                    sub = s.u8(); sl = s.uleb(); se = s.i + sl
                    t = R(b, s.i, se)
                    if sub == 1:
                        for _ in range(t.uleb()):
                            i = t.uleb(); func_names[i] = t.name()
                    elif sub == 7:
                        for _ in range(t.uleb()):
                            i = t.uleb(); global_names[i] = t.name()
                    s.i = se
        r.i = end
    # per function: global.set targets, calls, table / passive ops
    sets, calls, flags = [], [], []
    for (a, e) in bodies:
        st, cl, fl = {}, set(), set()

        def hit(kind, v, st=st, cl=cl, fl=fl):
            if kind == "global.set":
                st[v] = st.get(v, 0) + 1
            elif kind == "global.get":
                pass
            elif kind == "call":
                cl.add(v)
            elif kind in ("table", "passive", "exceptions"):
                fl.add((kind, v))
        scan_code(R(b, a, e), e, hit)
        sets.append(st); calls.append(cl); flags.append(fl)
    return dict(nfimp=nfimp, ngimp=ngimp, imports=imports, exports=exports, globals=globals_, start=start,
                sets=sets, calls=calls, flags=flags, host1=host1, mem=mem, func_names=func_names,
                global_names=global_names)


def reach(m, roots):
    seen, todo = set(), list(roots)
    while todo:
        f = todo.pop()
        if f in seen or f < m["nfimp"]:
            continue
        seen.add(f)
        todo.extend(m["calls"][f - m["nfimp"]])
    return seen


GSIZE = {0x7F: 4, 0x7E: 8, 0x7D: 4, 0x7C: 8, 0x7B: 16, 0x70: 8, 0x6F: 8}


def verdict(b):
    """(safe, why, g_off, g_len): g_off / g_len the globals' bytes in wasm2c 1.0.32's instance struct"""
    try:
        m = analyse(b)
    except Bad as e:
        return False, "unreadable: %s" % e, 0, 0
    if m["host1"]:
        return False, "a Fantasy CFW (rfm1_host_1: the file service)", 0, 0
    if any(k != 0 for (_, _, k) in m["imports"]):
        return False, "imports a global, memory or table", 0, 0
    nf = m["nfimp"]
    for f in range(nf, nf + len(m["flags"])):
        for (kind, what) in sorted(m["flags"][f - nf], key=str):
            name = m["func_names"].get(f, "func %d" % f)
            if kind == "table":
                return False, "%s in %s" % (what, name), 0, 0
            if kind == "passive":
                return False, "%s in %s" % (what, name), 0, 0
            if kind == "exceptions":
                return False, "exception handling in %s" % name, 0, 0
    if not m["mem"]:
        return False, "no memory of its own", 0, 0
    # the instance struct: a pointer per imported module (wasm2c: in the order first imported), then the globals
    mods = []
    for (mod, _, _) in m["imports"]:
        if mod not in mods:
            mods.append(mod)
    off = g_off = 8 * len(mods)
    for (t, mut, init) in m["globals"]:
        if t not in GSIZE:
            return False, "a global of type 0x%02x" % t, 0, 0
        sz = GSIZE[t]
        off = (off + sz - 1) // sz * sz
        off += sz
    g_len = off - g_off
    if g_len > 64:
        return False, "%d bytes of globals (the host keeps 64)" % g_len, 0, 0
    nmut = sum(1 for g in m["globals"] if g[1] == 1)
    why = "%d globals (%d mutable) at %d+%d, memory %d pages%s" % (
        len(m["globals"]), nmut, g_off, g_len, m["mem"][0],
        "" if m["mem"][1] == m["mem"][0] else " (max %s)" % m["mem"][1])
    return True, why, g_off, g_len


def main(argv):
    out = None
    if argv[:1] == ["--header"]:
        out, argv = argv[1], argv[2:]
    rows = {}
    for p in argv:
        b = open(p, "rb").read()
        ok, why, g_off, g_len = verdict(b)
        h = hashlib.sha256(b).hexdigest()
        print("%-5s %s %s: %s" % ("safe" if ok else "no", h[:12], p.split("/")[-1], why))
        if ok:
            rows[h] = (p.split("/")[-1], g_off, g_len)
    if out:
        with open(out + ".tmp", "w") as f:
            f.write("/* SPDX-License-Identifier: MPL-2.0 */\n/* generated by tools/snapsafe.py (make snapsafe): the .rfm1 "
                    "modules whose whole state between two calls is\n * their memory and their globals (no table or "
                    "segment changes after start-up, not a Fantasy CFW), with the place\n * of their globals in "
                    "wasm2c 1.0.32's instance struct: a snapshot restores them exactly (README \"The working memory\").\n"
                    " * Every other module keeps the flash-only state. */\n")
            f.write("typedef struct { const char *sha256; unsigned short g_off, g_len; } snapsafe_t;\n")
            f.write("static const snapsafe_t SNAPSAFE[] = {\n")
            for h in sorted(rows):
                n, go, gl = rows[h]
                f.write('    { "%s", %d, %d },   /* %s */\n' % (h, go, gl, n))
            f.write("};\n")
        import os
        os.replace(out + ".tmp", out)
    return 0





if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
