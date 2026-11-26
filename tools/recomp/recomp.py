#!/usr/bin/env python3
"""Static recompiler: ARM9 assembly of the Black 2 / White 2 disassembly -> C.

    python tools/recomp/recomp.py <pokeblack2 repo> <out dir> [--files main itcm ...]

Input: asm/*.s of nds197/pokeblack2 (functions delimited by thumb_func_start /
arm_func_start, labels carrying their address, jump tables written with
labels). Output: one C function per game function, working on the CPU state
of src/runtime/cpu.h, plus a dispatch table (address -> function) for
indirect calls. Instructions it cannot translate become UNIMPL() calls and
are counted (the feasibility measure of the trial).

Model:
  - registers and flags in a global struct; memory at native DS addresses;
  - bl/blx label -> direct C call (LR set to the return address, for code
    that saves it); blx rN, mov lr,pc + bx/ldr pc -> call_addr();
  - bx lr / pop {..,pc} / ldmia sp!,{..,pc} / mov pc,lr -> return;
  - branches inside the function -> goto; to another function -> tail call;
  - writes to pc (jump tables: add pc, r0) -> switch over the function's
    labels on the computed address;
  - flags are computed only when a later instruction may read them.
"""
import argparse
import json
import os
import re
import sys
from collections import defaultdict

REGS = {f"r{i}": i for i in range(16)}
REGS.update({"sb": 9, "sl": 10, "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15})
CONDS = ["eq", "ne", "hs", "cs", "lo", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al"]
CMAP = {"cs": "hs", "cc": "lo"}
SHIFTS = ("lsl", "lsr", "asr", "ror")
# lib/syscall: SVC_* wrappers = one BIOS call each (GBATEK DS BIOS numbers)
SVC = {"SVC_SoftReset": 0x00, "SVC_WaitByLoop": 0x03, "SVC_WaitIntr": 0x04, "SVC_WaitVBlankIntr": 0x05,
       "SVC_Halt": 0x06, "SVC_Div": 0x09, "SVC_DivRem": 0x09, "SVC_CpuSet": 0x0B, "SVC_CpuSetFast": 0x0C,
       "SVC_Sqrt": 0x0D, "SVC_GetCRC16": 0x0E, "SVC_IsDebugger": 0x0F, "SVC_UnpackBits": 0x10,
       "SVC_UncompressLZ8": 0x11, "SVC_UncompressLZ16FromDevice": 0x12, "SVC_UncompressHuffmanFromDevice": 0x13,
       "SVC_UncompressRL8": 0x14, "SVC_UncompressRL16FromDevice": 0x15, "SVC_GetSinTable": 0x1A,
       "SVC_GetPitchTable": 0x1B, "SVC_GetVolumeTable": 0x1C}

# ---------------------------------------------------------------- parsing


class Insn:
    __slots__ = ("op", "cond", "s", "suf", "args", "comment", "addr", "thumb", "text", "size")


def split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


ALU = ["adc", "add", "and", "bic", "eor", "mov", "mvn", "orr", "rsb", "rsc", "sbc", "sub",
       "cmp", "cmn", "tst", "teq", "neg", "mul", "mla", "lsl", "lsr", "asr", "ror",
       "smull", "umull", "smlal", "umlal", "clz",
       "smulbb", "smulbt", "smultb", "smultt", "smulwb", "smulwt",
       "smlabb", "smlabt", "smlatb", "smlatt", "smlawb", "smlawt",
       "qadd", "qsub", "qdadd", "qdsub"]
MEM = ["ldr", "str"]
OTHERS = ["ldm", "stm", "push", "pop", "swi", "svc", "mrs", "msr", "mcr", "mrc", "nop", "bkpt", "pld"]


def parse_mnemonic(m):
    """-> (base, cond, s, suffix) or None. Handles pre-UAL (ldreqb, ldmeqia,
    subges) and UAL (ldrbeq) spellings."""
    if m in ("bl", "blx", "blx_unaligned"):
        return ("blx" if m != "bl" else "bl"), "al", False, ""
    if m.startswith("bl") and m[2:] in CONDS:
        return "bl", CMAP.get(m[2:], m[2:]), False, ""
    if m.startswith("bx"):
        c = m[2:] or "al"
        return ("bx", CMAP.get(c, c), False, "") if c in CONDS else None
    if m.startswith("b") and (m == "b" or m[1:] in CONDS) and not m.startswith(("bic", "bkpt")):
        c = m[1:] or "al"
        return "b", CMAP.get(c, c), False, ""
    for base in sorted(ALU + MEM + OTHERS, key=len, reverse=True):
        if not m.startswith(base):
            continue
        rest = m[len(base):]
        for c in [""] + CONDS:
            for order in (0, 1):
                if order == 0 and rest.startswith(c):
                    tail = rest[len(c):]
                elif order == 1 and c and rest.endswith(c):
                    tail = rest[:-len(c)]
                else:
                    continue
                s = False
                suf = ""
                if base in ALU and tail == "s":
                    s, tail = True, ""
                if base in MEM and tail in ("", "b", "h", "sb", "sh", "d", "t", "bt"):
                    suf, tail = tail, ""
                if base in ("ldm", "stm") and tail in ("", "ia", "ib", "da", "db", "fd", "ed", "fa", "ea"):
                    suf, tail = tail or "ia", ""
                if tail == "":
                    cc = c or "al"
                    return base, CMAP.get(cc, cc), s, suf
    return None


def parse_file(path):
    """-> list of functions: {name, thumb, items: [('label', name, addr) |
    ('insn', Insn) | ('data', text)], addr}"""
    funcs = []
    cur = None
    mode = None
    addr = None
    in_text = True
    active = [True]           # #if defined(X) / #else / #endif (version blocks)
    # White 2 (--addr-map): the real address of each function, measured on
    # the binaries (tools/recomp/w2addr.py); labels follow their name plus the
    # function's shift until a version block changes the sizes, then count
    fid = os.path.splitext(os.path.basename(path))[0]
    fmap = ADDR_MAP.get(fid) if ADDR_MAP else None
    delta, cond_seen = 0, False
    for raw in open(path, encoding="utf-8", errors="replace"):
        line = raw.rstrip("\n")
        pp = line.strip()
        if pp.startswith("#"):
            cond_seen = True
            mi = re.match(r"^#if\s+defined\((\w+)\)\s*$", pp)
            if mi:
                active.append(active[-1] and mi.group(1) in DEFINES)
            elif pp.startswith("#else"):
                parent = active[-2] if len(active) > 1 else True
                active[-1] = parent and not active[-1]
            elif pp.startswith("#endif"):
                active.pop()
            elif pp.startswith("#include"):
                pass                  # config.h: only the version macros used above
            else:
                raise SystemExit(f"{path}: unsupported preprocessor line: {pp}")
            continue
        if not active[-1]:
            continue
        code, _, comment = line.partition(";")
        s = code.strip()
        comment = comment.strip()
        if not s:
            continue
        m = re.match(r"^(?:non_word_aligned_)?(thumb|arm)_func_start\s+(\w+)", s)
        if m:
            mode = m.group(1) == "thumb"
            if addr is not None and not s.startswith("non_word"):
                addr = (addr + 3) & ~3
            cur = {"name": m.group(2), "thumb": mode, "items": [], "addr": None}
            funcs.append(cur)
            continue
        if re.match(r"^(thumb|arm)_func_end", s):
            continue
        if s.startswith((".text", ".section", ".data", ".bss", ".rodata")):
            in_text = s.startswith(".text") or (".text" in s) or (".itcm" in s) or ("text" in s and ".section" in s)
            if not in_text:
                cur = None
            continue
        # label (possibly followed by a directive on the same line)
        m = re.match(r"^([A-Za-z_.$][\w.$]*):\s*(.*)$", s)
        if m:
            name, after = m.group(1), m.group(2)
            la = None
            mm = re.match(r"^_?(?:FUN_|sub_|ov\d+_)?([0-9A-Fa-f]{8})$", name)
            if mm:
                la = int(mm.group(1), 16)
            mc = re.search(r"0x([0-9A-Fa-f]{8})", comment)
            if la is None and mc:
                la = int(mc.group(1), 16)
            if SEQ_CHECK is not None and la is not None and addr is not None:
                SEQ_CHECK.append((name, la, addr))
            # SEQUENTIAL (White 2): addresses come from the sizes of what is
            # assembled, not from the names (which are Black 2's); only a far
            # jump (a new region: bss, another module) resyncs on a name
            if fmap is not None and la is not None:
                if cur is not None and name == cur["name"] and name in fmap:
                    addr = fmap[name]             # a function: measured
                    delta = addr - la
                    cond_seen = False
                elif cur is None or not cond_seen or addr is None:
                    addr = la + delta
                # else: after a version block inside the function, counted
            elif la is not None and (not SEQUENTIAL or addr is None or abs(la - addr) > SEQ_RESYNC):
                addr = la
            if cur is not None:
                cur["items"].append(("label", name, addr))
                if cur["addr"] is None and name == cur["name"]:
                    cur["addr"] = addr
            if after:
                s = after.strip()
            else:
                continue
        if s.startswith("."):
            size = directive_size(s, addr)
            if cur is not None:
                cur["items"].append(("data", s))
            if addr is not None:
                if s.startswith((".balign", ".align")):
                    n = int(s.split()[1].split(",")[0], 0)
                    n = n if s.startswith(".balign") else (1 << n)
                    addr = (addr + n - 1) & ~(n - 1)
                else:
                    addr += size
            continue
        if cur is None:
            continue
        m = re.match(r"^([a-z][a-z0-9_.]*)\s*(.*)$", s)
        if not m:
            continue
        ins = Insn()
        ins.text = s
        ins.comment = comment
        ins.thumb = mode
        ins.addr = addr
        pm = parse_mnemonic(m.group(1))
        if pm is None:
            ins.op, ins.cond, ins.s, ins.suf = "?", "al", False, ""
        else:
            ins.op, ins.cond, ins.s, ins.suf = pm
        ins.args = split_args(m.group(2))
        ins.size = 4 if (not mode or ins.op in ("bl",) or (ins.op == "blx" and not ins.args[0] in REGS)) else 2
        cur["items"].append(("insn", ins))
        if addr is not None:
            addr += ins.size
    for f in funcs:
        if f["addr"] is None:
            mm = re.search(r"([0-9A-Fa-f]{8})$", f["name"])
            f["addr"] = int(mm.group(1), 16) if mm else None
    return funcs


def directive_size(s, addr):
    d = s.split()[0]
    rest = s[len(d):].strip()
    if d in (".word", ".4byte", ".long"):
        return 4 * len(split_args(rest))
    if d in (".short", ".hword", ".2byte"):
        return 2 * len(split_args(rest))
    if d == ".byte":
        return len(split_args(rest))
    if d in (".asciz", ".string"):
        m = re.match(r'^"(.*)"$', rest)
        if m:
            return len(m.group(1).encode("latin-1", "replace").decode("unicode_escape").encode("latin-1", "replace")) + 1
    if d == ".ascii":
        m = re.match(r'^"(.*)"$', rest)
        if m:
            return len(m.group(1).encode("latin-1", "replace").decode("unicode_escape").encode("latin-1", "replace"))
    if d in (".space", ".skip", ".zero"):
        return int(rest.split(",")[0], 0)
    return 0


# ---------------------------------------------------------- translation

class Untranslatable(Exception):
    pass


def reg(x):
    x = x.strip().rstrip("!").lower()
    if x in REGS:
        return REGS[x]
    raise Untranslatable(f"register {x}")


# Game/SDK functions whose body the runtime provides (src/runtime/hle.c):
# spin-waits on the ARM7 and the thread context switches.
HLE = {
    "FUN_0203CB94": "game assertion handler (message logged, then fatal)",
    "FUN_0207CA48": "PXI_IsCallbackReady: the reproduced ARM7 is always ready",
    "FUN_01FF8CE0": "thread switch in the IRQ handler (context save/load ^)",
    "FUN_0207ABBC": "OS_LoadContext (ldmia ^)",
    "FUN_0207AB70": "OS_SaveContext (returns twice: host fibers)",
    "OS_Terminate": "logged (where the game stopped), then halts like the original",
}

# functions that tell the runtime what the game does (logging hooks called at
# their entry, the function itself is translated as usual)
HOOKS = {
    "FUN_0207113C": "hook_overlay_start",     # FS_StartOverlay-like: R0 = overlay info, [0] = id
}

SEQUENTIAL = False       # addresses from sizes (White 2), see parse_file
SEQ_RESYNC = 0x1000      # a bigger gap between computed and named address: new region
SEQ_CHECK = None         # list collecting (label, named address, computed address)
ADDR_MAP = None          # {module: {function: White 2 address}} (--addr-map)
DEFINES = {"BLACK2"}           # asm "#if defined(...)" blocks; WHITE2 with --version white2
CONSTS = {"GAME_VERSION": 23}      # VERSION_BLACK2 (22 = White 2), set by --version


def imm(x):
    x = x.strip()
    if x.startswith("#"):
        x = x[1:]
    for k, v in CONSTS.items():
        x = x.replace(k, str(v))
    if re.fullmatch(r"[0-9a-fA-FxX+\- ()*]+", x):
        return eval(x, {"__builtins__": {}}) & 0xFFFFFFFF
    return int(x, 0) & 0xFFFFFFFF


def is_imm(x):
    return x.strip().startswith("#")


def rv(n, ins):
    """value of register n as an operand (pc reads ahead)"""
    if n == 15:
        return f"0x{(ins.addr + (4 if ins.thumb else 8)) & 0xFFFFFFFF:08X}u"
    return f"R({n})"


class FuncGen:
    func_names = set()       # every function name of the asm
    continuations = set()    # functions that are the split-off rest of the previous one
    inner_labels = set()     # labels inside functions (not function names)
    file_labels = {}         # {file: {label: address}} as parsed (White 2: measured)

    def __init__(self, f, cname, resolver, fileid):
        self.f = f
        self.cname = cname
        self.resolve = resolver
        self.fileid = fileid
        self.labels = {}          # name -> addr
        self.label_addr = {}      # addr -> C label
        for it in f["items"]:
            if it[0] == "label":
                self.labels[it[1]] = it[2]
                if it[2] is not None:
                    self.label_addr.setdefault(it[2], "L_" + it[1].replace(".", "_"))
        # "add pc, pc, rN": a computed jump into the middle of unrolled code
        # (division, fill loops): every instruction becomes a jump target
        self.computed = any(it[0] == "insn" and it[1] is not None and
                            re.match(r"^add\w*\s+pc\s*,\s*pc\s*,\s*(r\d|ip|sb|sl|fp|lr)", it[1].text.strip())
                            for it in f["items"])
        if self.computed:
            for it in f["items"]:
                if it[0] == "insn" and it[1] is not None and it[1].addr is not None:
                    self.label_addr.setdefault(it[1].addr, f"L_i_{it[1].addr:08X}")
        self.out = []
        self.unimpl = 0
        self.popped = set()
        # DSProt (anti-piracy, lib/dsprot): the game's inline wrapper hashes
        # the code of a detection function before calling it and calls the
        # "fail" callback on a mismatch. The detection code is still encoded
        # here (DSProt_DecodeFunctions is answered by the runtime), so the
        # hash compare is made to succeed, and the pool words holding the
        # callback table and index are passed to dsprot_call, which runs the
        # "pass" callback like a genuine cartridge.
        self.dsprot = {}          # "DSProt_CallbackTable"/"Index" -> pool address
        self.dsprot_func = False
        self.pool_vals = {}       # pool address -> numeric .word
        self.icount = 0
        last = None
        for it in f["items"]:
            if it[0] == "label":
                last = it[2]
            elif it[0] == "data":
                m = re.match(r"^\.word\s+(DSProt_\w+)\s*$", it[1].strip())
                if m and last is not None:
                    self.dsprot[m.group(1)] = last
                    self.dsprot_func = True
                m = re.match(r"^\.word\s+(0x[0-9A-Fa-f]{8})\s*$", it[1].strip())
                if m and last is not None:
                    self.pool_vals[last] = int(m.group(1), 16)
                if last is not None:
                    last += 4
        self.lit_reg = {}         # register -> instruction index of its last literal load

    # ---- flag liveness (backward over the linear code, conservative)
    def liveness(self, insns):
        if self.computed:
            return [ins is not None and self.sets_flags(ins) for ins in insns]
        needed = [False] * len(insns)
        live = False
        for i in range(len(insns) - 1, -1, -1):
            ins = insns[i]
            if ins is None:              # label: keep (fall-through join)
                continue
            op = ins.op
            if op in ("bl", "blx") or (op == "bx" and ins.cond == "al"):
                live = False
            elif op == "b" and ins.cond == "al":
                live = True
            sets = self.sets_flags(ins)
            reads = ins.cond != "al" or op in ("adc", "sbc", "rsc") or ("rrx" in ins.text)
            if sets:
                needed[i] = live
                full = op in ("cmp", "cmn", "add", "sub", "adc", "sbc", "rsb", "rsc", "neg") and ins.cond == "al"
                live = reads or (live and not full)
            else:
                live = live or reads
        return needed

    def sets_flags(self, ins):
        op = ins.op
        if op in ("cmp", "cmn", "tst", "teq"):
            return True
        if ins.thumb:
            if op in ("and", "eor", "lsl", "lsr", "asr", "ror", "adc", "sbc", "neg", "orr", "mul", "bic", "mvn"):
                return True
            if op in ("add", "sub", "mov"):
                regs = [a for a in ins.args if a.strip().lower() in REGS]
                hi = any(REGS[a.strip().lower()] >= 8 for a in regs)
                if op == "mov" and len(ins.args) == 2 and not is_imm(ins.args[1]) and hi:
                    return False
                if op in ("add", "sub") and hi:
                    return False
                return True
            return False
        return ins.s

    # ---- emit helpers
    def emit(self, s):
        self.out.append("    " + s)

    def cond_wrap(self, ins, body):
        if ins.cond == "al":
            for b in body:
                self.emit(b)
        else:
            self.emit(f"if (C_{ins.cond.upper()}) {{")
            for b in body:
                self.emit("    " + b)
            self.emit("}")

    def branch_target(self, name, ins):
        if name in self.labels and self.labels[name] is not None and name != self.f["name"]:
            return ("goto", self.label_addr[self.labels[name]])
        target = self.resolve(name, self.fileid)
        return ("call", target)

    def op2(self, args, ins, f):
        """ARM operand 2 / Thumb register operand -> C expression"""
        if len(args) == 1:
            a = args[0]
            if is_imm(a):
                return f"0x{imm(a):08X}u"
            return rv(reg(a), ins)
        base = rv(reg(args[0]), ins)
        sh = args[1].strip().lower()
        if sh == "rrx":
            return f"rrx_c({base}, {int(f)})"
        m = re.match(r"^(lsl|lsr|asr|ror)\s+(.+)$", sh)
        if not m:
            raise Untranslatable("shift " + sh)
        kind, amount = m.group(1), m.group(2)
        if amount.startswith("#"):
            n = imm(amount)
            if kind == "lsl":
                return f"lsl_c({base}, {n}u, {int(f)})" if f else (f"({base} << {n})" if n else base)
            if kind == "lsr":
                return f"lsr_i({base}, {n}u, {int(f)})"
            if kind == "asr":
                return f"asr_i({base}, {n}u, {int(f)})"
            return f"ror_c({base}, {n}u, {int(f)})"
        return f"{kind}_c({base}, {rv(reg(amount), ins)} & 0xff, {int(f)})"

    def mem_addr(self, a, ins):
        """[rn, ...] forms -> (pre-address expr, writeback stmt or None)"""
        a = a.strip()
        m = re.match(r"^\[(\w+)\]$", a)
        if m:
            return rv(reg(m.group(1)), ins), None
        m = re.match(r"^\[(\w+),\s*(.+?)\](!?)$", a)
        if m:
            rn = reg(m.group(1))
            off = self.offset_expr(m.group(2), ins)
            e = f"({rv(rn, ins)} + {off})"
            return e, (f"R({rn}) = {e};" if m.group(3) else None)
        raise Untranslatable("address " + a)

    def offset_expr(self, o, ins):
        o = o.strip()
        if o.startswith("#"):
            v = int(o[1:], 0)
            return f"(u32){v}"
        neg = o.startswith("-")
        if neg:
            o = o[1:]
        parts = split_args(o)
        e = self.op2(parts, ins, False)
        return f"(u32)-(s32)({e})" if neg else e

    # ---- per instruction
    def translate(self, ins, need_flags):
        self.icount += 1
        op, a = ins.op, ins.args
        f = need_flags
        T = ins.thumb
        A = len(a)
        if op == "nop" or op == "pld":
            return
        if op == "b":
            kind, t = self.branch_target(a[0], ins)
            if kind == "goto":
                tgt = self.labels.get(a[0].strip())
                if tgt is not None and ins.addr is not None and tgt <= ins.addr:
                    # a loop: time passes (IRQs, scanlines) as on the DS
                    self.cond_wrap(ins, ["LOOP_TICK();", f"goto {t};"])
                else:
                    self.cond_wrap(ins, [f"goto {t};"])
            else:
                self.cond_wrap(ins, [f"{t}; return;"])
            return
        if op in ("bl", "blx") and a[0].strip().lower() not in REGS:
            ret = (ins.addr + 4) | (1 if T else 0)
            name = a[0].strip()
            if name in self.labels and self.labels[name] is not None and name != self.f["name"] \
                    and not re.match(r"^(FUN|sub)_", name):
                self.cond_wrap(ins, [f"LR = 0x{ret:08X}u;", f"goto {self.label_addr[self.labels[name]]};"])
                return
            # Thumb "bl" as a long branch (b reaches only +-2 KiB): the compiler
            # jumps to a label of a function the disassembler split off, or back
            # to the start of such a continuation; the stack frame is the caller's
            if T and op == "bl":
                if name == self.f["name"] and name in FuncGen.continuations:
                    self.cond_wrap(ins, [f"LR = 0x{ret:08X}u;", "LOOP_TICK();",
                                         f"goto {self.label_addr[self.labels[name]]};"])
                    return
                if (name in FuncGen.inner_labels and name not in self.labels) or name in FuncGen.continuations:
                    t = self.resolve(name, self.fileid)
                    self.cond_wrap(ins, [f"LR = 0x{ret:08X}u;", f"{t}; return;"])
                    return
            t = self.resolve(name, self.fileid)
            if t.startswith("dsprot_call(") and "DSProt_CallbackTable" in self.dsprot and \
                    "DSProt_CallbackIndex" in self.dsprot:
                # the pool words that hold the callback table and its index
                t = t.replace(", 0, 0)", f", 0x{self.dsprot['DSProt_CallbackTable']:08X}u,"
                                         f" 0x{self.dsprot['DSProt_CallbackIndex']:08X}u)")
            self.cond_wrap(ins, [f"LR = 0x{ret:08X}u;", f"{t};"])
            return
        if op == "blx":
            n = reg(a[0])
            ret = (ins.addr + 2) | 1 if T else ins.addr + 4
            self.cond_wrap(ins, [f"{{ u32 t_ = R({n}); LR = 0x{ret:08X}u; call_addr(t_); }}"])
            return
        if op == "bx":
            n = reg(a[0])
            if n == 14 or n in self.popped:
                self.cond_wrap(ins, ["return;"])
            else:
                self.cond_wrap(ins, [f"call_addr(R({n})); return;"])
            return
        if op in ("push", "pop") or op in ("ldm", "stm"):
            return self.ldm_stm(ins)
        if op in ("ldr", "str"):
            return self.ldr_str(ins)
        if op in ("swi", "svc"):
            n = imm(a[0]) if a else 0
            if not T:
                n >>= 16 if n > 0xFF else 0
            self.cond_wrap(ins, [f"swi_call(0x{n:X});"])
            return
        if op == "mrs":
            self.cond_wrap(ins, [f"R({reg(a[0])}) = cpsr_read();"])
            return
        if op == "msr":
            field = a[0].strip().lower()
            mask = 0
            if "_" in field:
                for ch in field.split("_")[1]:
                    mask |= {"c": 0xFF, "x": 0xFF00, "s": 0xFF0000, "f": 0xFF000000}[ch]
            else:
                mask = 0xFF0000FF
            if field.startswith("spsr"):
                self.cond_wrap(ins, [f"g.spsr = (g.spsr & ~0x{mask:08X}u) | ({self.op2(a[1:], ins, False)} & 0x{mask:08X}u);"])
            else:
                self.cond_wrap(ins, [f"cpsr_write({self.op2(a[1:], ins, False)}, 0x{mask:08X}u);"])
            return
        if op in ("mcr", "mrc"):
            # p15, op1, rd, cn, cm, op2
            rd = reg(a[2])
            cn, cm = int(a[3].strip()[1:]), int(a[4].strip()[1:])
            op2 = int(a[5], 0) if A > 5 else 0
            if op == "mcr":
                self.cond_wrap(ins, [f"cp15_write({cn}, {cm}, {op2}, R({rd}));"])
            else:
                self.cond_wrap(ins, [f"R({rd}) = cp15_read({cn}, {cm}, {op2});"])
            return
        if op == "bkpt":
            self.cond_wrap(ins, [f'UNIMPL("{self.cname}", "bkpt");'])
            return
        return self.alu(ins, f)

    def set_pc(self, ins, expr):
        """a write to pc: return, or computed jump inside the function"""
        cases = "".join(f"case 0x{ad:08X}u: goto {lb}; " for ad, lb in sorted(self.label_addr.items()))
        return [f"{{ u32 t_ = ({expr}) & ~1u; switch (t_) {{ {cases}default: call_addr(t_ | {1 if ins.thumb else 0}); return; }} }}"]

    def alu(self, ins, f):
        op, a, T = ins.op, ins.args, ins.thumb
        A = len(a)
        body = []
        if op in ("cmp", "cmn", "tst", "teq"):
            x = rv(reg(a[0]), ins)
            y = self.op2(a[1:], ins, op in ("tst", "teq") and f)
            if op == "cmp" and self.dsprot_func and len(a) == 2 and not is_imm(a[1]) and \
                    any(self.icount - self.lit_reg.get(reg(r), -99) <= 3 for r in a):
                body.append(f"sub_f({x}, {x});   /* DSProt integrity hash: as on a genuine cartridge */")
            elif op == "cmp":
                body.append(f"sub_f({x}, {y});")
            elif op == "cmn":
                body.append(f"add_f({x}, {y});")
            elif op == "tst":
                body.append(f"SET_NZ({x} & {y});")
            else:
                body.append(f"SET_NZ({x} ^ {y});")
            self.cond_wrap(ins, body if f else [])
            return
        if op in ("mul", "mla"):
            if T:            # mul rd, rs : rd = rd * rs
                d, s_ = reg(a[0]), reg(a[1])
                e = f"R({d}) * R({s_})"
            else:
                d = reg(a[0])
                e = f"R({reg(a[1])}) * R({reg(a[2])})" + (f" + R({reg(a[3])})" if op == "mla" else "")
            body.append(f"R({d}) = {e};")
            if f:
                body.append(f"SET_NZ(R({d}));")
            self.cond_wrap(ins, body)
            return
        if op in ("smull", "umull", "smlal", "umlal"):
            lo, hi, x, y = (reg(v) for v in a[:4])
            sgn = op[0] == "s"
            prod = f"((s64)(s32)R({x}) * (s64)(s32)R({y}))" if sgn else f"((u64)R({x}) * (u64)R({y}))"
            if op.endswith("lal"):
                prod = f"({prod} + (s64)(((u64)R({hi}) << 32) | R({lo})))"
            body.append(f"{{ u64 p_ = (u64){prod}; R({lo}) = (u32)p_; R({hi}) = (u32)(p_ >> 32);"
                        + (f" g.n = R({hi}) >> 31; g.z = !p_;" if f else "") + " }")
            self.cond_wrap(ins, body)
            return
        if op == "clz":
            self.cond_wrap(ins, [f"R({reg(a[0])}) = clz32(R({reg(a[1])}));"])
            return
        if op.startswith(("smul", "smla")):
            d = reg(a[0])
            x, y = reg(a[1]), reg(a[2])
            if op in ("smulwb", "smulwt", "smlawb", "smlawt"):
                half = f"(s16)(R({y}) {'>> 16' if op[-1] == 't' else ''})"
                e = f"(u32)(((s64)(s32)R({x}) * {half}) >> 16)"
            else:
                hx = f"(s16)(R({x}) {'>> 16' if op[4] == 't' else ''})"
                hy = f"(s16)(R({y}) {'>> 16' if op[5] == 't' else ''})"
                e = f"(u32)((s32){hx} * (s32){hy})"
            if op.startswith("smla"):
                e = f"({e} + R({reg(a[3])}))"
            self.cond_wrap(ins, [f"R({d}) = {e};"])
            return
        if op in ("qadd", "qsub", "qdadd", "qdsub"):
            d, x, y = reg(a[0]), reg(a[1]), reg(a[2])
            yy = f"(s64)sat32(2 * (s64)(s32)R({y}))" if op.startswith("qd") else f"(s64)(s32)R({y})"
            sign = "+" if "add" in op else "-"
            self.cond_wrap(ins, [f"R({d}) = (u32)sat32((s64)(s32)R({x}) {sign} {yy});"])
            return
        # data processing
        d = reg(a[0])
        if op == "neg":
            src, rest = rv(reg(a[1]), ins), None
            e = f"sub_f(0, {src})" if f else f"(u32)-(s32){src}"
        elif T and op in ("lsl", "lsr", "asr", "ror"):
            if A == 3:     # lsl rd, rm, #imm
                n = imm(a[2])
                fn = {"lsl": "lsl_c", "lsr": "lsr_i", "asr": "asr_i", "ror": "ror_c"}[op]
                e = f"{fn}({rv(reg(a[1]), ins)}, {n}u, {int(f)})"
            else:          # lsl rd, rs
                e = f"{op}_c(R({d}), R({reg(a[1])}) & 0xff, {int(f)})"
            if f:
                e = f"({{ u32 r_ = {e}; SET_NZ(r_); r_; }})"
        elif not T and op in ("lsl", "lsr", "asr", "ror"):    # UAL alias in ARM code
            e = self.op2([a[1], f"{op} {a[2]}"], ins, f)
            if f:
                e = f"({{ u32 r_ = {e}; SET_NZ(r_); r_; }})"
        else:
            if op in ("mov", "mvn"):
                src = a[1:]
                x = None
            elif A == 2 and op in ("add", "sub", "adc", "sbc", "and", "orr", "eor", "bic", "rsb"):
                x = rv(d, ins)        # thumb two-operand form
                src = a[1:]
                if T and op == "add" and d == 15:
                    pass
            else:
                x = rv(reg(a[1]), ins)
                src = a[2:]
            logical = op in ("mov", "mvn", "and", "orr", "eor", "bic")
            y = self.op2(src, ins, f and logical)
            if T and op in ("add", "sub") and x is not None and a[1].strip().lower() == "sp" and A == 3:
                pass
            if op == "mov":
                e = y
            elif op == "mvn":
                e = f"~{y}"
            elif op == "and":
                e = f"({x} & {y})"
            elif op == "orr":
                e = f"({x} | {y})"
            elif op == "eor":
                e = f"({x} ^ {y})"
            elif op == "bic":
                e = f"({x} & ~{y})"
            elif op == "add":
                e = f"add_f({x}, {y})" if f else f"({x} + {y})"
            elif op == "sub":
                e = f"sub_f({x}, {y})" if f else f"({x} - {y})"
            elif op == "rsb":
                e = f"sub_f({y}, {x})" if f else f"({y} - {x})"
            elif op == "adc":
                e = f"adc_f({x}, {y})" if f else f"adc({x}, {y})"
            elif op == "sbc":
                e = f"sbc_f({x}, {y})" if f else f"sbc({x}, {y})"
            elif op == "rsc":
                e = f"sbc_f({y}, {x})" if f else f"sbc({y}, {x})"
            else:
                raise Untranslatable(op)
            if f and logical:
                e = f"({{ u32 r_ = {e}; SET_NZ(r_); r_; }})"
        if d == 15:
            if op == "mov" and a[1].strip().lower() == "lr":
                self.cond_wrap(ins, ["return;"])
            else:
                self.cond_wrap(ins, self.set_pc(ins, e))
            return
        # a copy of lr holds the return address: "bx" to it is a return
        if op == "mov" and len(a) == 2 and a[1].strip().lower() == "lr":
            self.popped.add(d)
        elif d in self.popped and d != 14:
            self.popped.discard(d)
        self.cond_wrap(ins, [f"R({d}) = {e};"])

    def ldr_str(self, ins):
        op, a, suf, T = ins.op, ins.args, ins.suf, ins.thumb
        rd = reg(a[0])
        load = op == "ldr"
        size = {"": 32, "b": 8, "h": 16, "sb": 8, "sh": 16, "d": 64, "t": 32, "bt": 8}[suf]
        if suf == "d":
            raise Untranslatable("ldrd/strd")
        signed = suf in ("sb", "sh")
        # literal: ldr rd, label
        if load and len(a) == 2 and not a[1].strip().startswith("["):
            lab = a[1].strip().lstrip("=")
            la = None
            mm = re.search(r"([0-9A-Fa-f]{8})$", lab)
            if lab in self.labels and self.labels[lab] is not None:
                la = self.labels[lab]
            elif FuncGen.file_labels.get(self.fileid, {}).get(lab) is not None:
                la = FuncGen.file_labels[self.fileid][lab]   # a pool of another function
            elif mm:
                la = int(mm.group(1), 16)
            if la is None:
                raise Untranslatable("literal " + lab)
            v = self.pool_vals.get(la)
            if v is not None and v >= 0x01000000 and not 0x01FF0000 <= v < 0x03000000:
                self.lit_reg[rd] = self.icount       # a hash-like constant (DSProt check)
            body = [f"R({rd}) = rd32(0x{la:08X}u);"] if rd != 15 else self.set_pc(ins, f"rd32(0x{la:08X}u)")
            self.cond_wrap(ins, body)
            return
        # post-indexed: [rn], #imm / [rn], rm
        if len(a) >= 3 and re.match(r"^\[\w+\]$", a[1].strip()):
            rn = reg(a[1].strip()[1:-1])
            addr = f"R({rn})"
            wb = f"R({rn}) = R({rn}) + {self.offset_expr(', '.join(a[2:]), ins)};"
        else:
            addr, wb = self.mem_addr(", ".join(a[1:]), ins)
        rdfn = {32: "rd32r" if not T else "rd32", 16: "rds16" if signed else "rd16", 8: "rds8" if signed else "rd8"}[size]
        wrfn = {32: "wr32", 16: "wr16", 8: "wr8"}[size]
        body = []
        if load:
            body.append(f"{{ u32 a_ = {addr}; u32 v_ = (u32){rdfn}(a_);")
            if wb:
                body.append("  " + wb)
            if rd == 15:
                if "sp" in a[1].lower() and wb:      # ldr pc, [sp], #4 : return
                    body.append("  return; }")
                else:
                    body.extend(["  " + s for s in self.set_pc(ins, "v_")])
                    body.append("}")
            else:
                body.append(f"  R({rd}) = v_; }}")
        else:
            val = rv(rd, ins) if rd != 15 else f"0x{(ins.addr + 12) & 0xFFFFFFFF:08X}u"
            body.append(f"{{ u32 a_ = {addr}; {wrfn}(a_, {val});" + (f" {wb}" if wb else "") + " }")
        self.cond_wrap(ins, body)

    def reglist(self, s):
        s = s.strip()
        if not (s.startswith("{") and s.endswith("}")):
            raise Untranslatable("reglist " + s)
        out = []
        for p in s[1:-1].split(","):
            p = p.strip()
            if not p:
                continue
            if "-" in p:
                x, y = p.split("-")
                out.extend(range(reg(x), reg(y) + 1))
            else:
                out.append(reg(p))
        return sorted(set(out))

    def ldm_stm(self, ins):
        op, a = ins.op, ins.args
        if op == "push":
            base, wb, mode, regs, load = 13, True, "db", self.reglist(a[0]), False
        elif op == "pop":
            base, wb, mode, regs, load = 13, True, "ia", self.reglist(a[0]), True
        else:
            b = a[0].strip()
            wb = b.endswith("!")
            base = reg(b)
            regs = self.reglist(a[1])
            load = op == "ldm"
            mode = {"fd": "ia" if load else "db", "ea": "db" if load else "ia",
                    "fa": "da" if load else "ib", "ed": "ib" if load else "da"}.get(ins.suf, ins.suf)
        n = len(regs)
        start = {"ia": "0", "ib": "4", "da": f"(u32)-{4 * n - 4}", "db": f"(u32)-{4 * n}"}[mode]
        final = {"ia": f"{4 * n}", "ib": f"{4 * n}", "da": f"(u32)-{4 * n}", "db": f"(u32)-{4 * n}"}[mode]
        body = [f"{{ u32 a_ = R({base}) + {start};"]
        ret = False
        for i, r in enumerate(regs):
            if load:
                if r == 15:
                    ret = True
                    continue
                body.append(f"  R({r}) = rd32(a_ + {4 * i});")
                self.popped.add(r)
            else:
                val = f"R({r})" if r != 15 else f"0x{(ins.addr + 12) & 0xFFFFFFFF:08X}u"
                body.append(f"  wr32(a_ + {4 * i}, {val});")
        if wb and not (load and base in regs):
            body.append(f"  R({base}) = R({base}) + {final};")
        if ret:
            body.append("  return;")
        body.append("}")
        self.cond_wrap(ins, body)

    def generate(self):
        base = self.f["name"]
        if base in HLE:
            return [f"void {self.cname}(void) {{",
                    f"    /* replaced by the runtime: {HLE[base]} */",
                    f"    hle_{self.cname}();",
                    "}"]
        items = self.f["items"]
        seq = [it[1] if it[0] == "insn" else None for it in items]
        insns = [s for s in seq]
        needed = self.liveness(insns)
        self.out.append(f"void {self.cname}(void) {{")
        a = self.f["addr"]
        self.out.append(f"    /* 0x{a:08X} {'thumb' if self.f['thumb'] else 'arm'} */" if a is not None else "    /* ? */")
        if a is not None:
            self.out.append(f"    FN_ENTER(0x{a | (1 if self.f['thumb'] else 0):08X}u);")
            if base in HOOKS:
                self.out.append(f"    {HOOKS[base]}();")
        entry = getattr(self, "entry_label", None)
        if entry:
            # secondary entry: another function branches to this label
            self.out.append(f"    goto {self.label_addr[self.labels[entry]]};")
        prev_mov_lr_pc = False
        prev_ldr_lr = False
        emitted = set()
        # veneer "ldr ip, [pc] ; bx ip" written as bytes, target in the next
        # .word (reached in ARM, or from Thumb through "bx pc ; nop"): a jump
        veneer_target = None
        for k, it in enumerate(items):
            if it[0] == "data" and it[1].startswith(".byte"):
                vals, j = [], k     # the 8 bytes may span several .byte lines
                try:
                    while j < len(items) and items[j][0] == "data" and items[j][1].startswith(".byte") and len(vals) < 8:
                        vals += [int(v, 0) for v in split_args(items[j][1].split(None, 1)[1])]
                        j += 1
                except ValueError:
                    continue
                if vals[:8] == [0x00, 0xC0, 0x9F, 0xE5, 0x1C, 0xFF, 0x2F, 0xE1]:
                    nxt_it = next((x for x in items[j:] if x[0] == "data"), None)
                    mw = re.match(r"^\.word\s+(\S+)\s*$", nxt_it[1]) if nxt_it else None
                    if mw:
                        veneer_target = mw.group(1)
                    break
        veneer_call = None
        if veneer_target:
            mh = re.match(r"^0x([0-9A-Fa-f]+)$", veneer_target)
            veneer_call = f"call_addr(0x{mh.group(1)}u)" if mh else self.resolve(veneer_target, self.fileid)
        for idx, it in enumerate(items):
            if it[0] == "label":
                lb = "L_" + it[1].replace(".", "_")
                if it[2] is not None and self.label_addr.get(it[2]) == lb and lb not in emitted:
                    emitted.add(lb)
                    self.out.append(f"{lb}:;")
                continue
            if it[0] == "data":
                d = it[1]
                # SWP / SWPB written as raw words (the original assembler lacked them)
                mw = re.match(r"^\.word\s+(0x[0-9A-Fa-f]{8})\s*$", d)
                if mw and not self.f["thumb"]:
                    v = int(mw.group(1), 16)
                    if (v & 0x0FB00FF0) == 0x01000090:
                        rn, rd, rm = (v >> 16) & 15, (v >> 12) & 15, v & 15
                        byte = (v >> 22) & 1
                        rdf, wrf = ("rd8", "wr8") if byte else ("rd32", "wr32")
                        body = [f"{{ u32 a_ = R({rn}); u32 t_ = {rdf}(a_); {wrf}(a_, R({rm})); R({rd}) = t_; }}"]
                        cond = v >> 28
                        if cond != 0xE:
                            names = ["EQ", "NE", "HS", "LO", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE"]
                            body = [f"if (C_{names[cond]}) " + body[0]]
                        self.emit(f"/* swp{'b' if byte else ''} (raw 0x{v:08X}) */")
                        for b in body:
                            self.emit(b)
                        continue
                if d.startswith(".byte") and veneer_call and not self.f["thumb"]:
                    vals = [int(v, 0) for v in split_args(d.split(None, 1)[1])]
                    if vals[:8] == [0x00, 0xC0, 0x9F, 0xE5, 0x1C, 0xFF, 0x2F, 0xE1]:
                        self.emit(f"/* veneer: ldr ip, [pc]; bx ip -> {veneer_target} */")
                        self.emit(f"{veneer_call}; return;")
                continue
            ins = it[1]
            if self.computed and ins is not None and ins.addr is not None:
                lb = self.label_addr.get(ins.addr)
                if lb and lb.startswith("L_i_") and lb not in emitted:
                    emitted.add(lb)
                    self.out.append(f"{lb}:;")
            try:
                if veneer_call and ins.op == "bx" and ins.args and ins.args[0].strip().lower() == "pc" and ins.thumb:
                    self.emit(f"/* bx pc -> ARM veneer -> {veneer_target} */")
                    self.emit(f"{veneer_call}; return;")
                    continue
                # ARM indirect call idiom: mov lr, pc ; bx rN / ldr pc, [...]
                if prev_mov_lr_pc and ins.op == "bx" and ins.cond == "al":
                    n = reg(ins.args[0])
                    self.emit(f"{{ u32 t_ = R({n}); LR = 0x{ins.addr + 4:08X}u; call_addr(t_); }}")
                    prev_mov_lr_pc = False
                    continue
                if prev_mov_lr_pc and ins.op == "ldr" and reg(ins.args[0]) == 15:
                    addr, wb = self.mem_addr(", ".join(ins.args[1:]), ins)
                    self.emit(f"{{ u32 t_ = rd32({addr}); LR = 0x{ins.addr + 4:08X}u; call_addr(t_); }}")
                    prev_mov_lr_pc = False
                    continue
                # "ldr lr, =X ; bx rN": rN returns to X, then X runs (IRQ handler epilogue)
                if prev_ldr_lr and ins.op == "bx" and ins.cond == "al" and reg(ins.args[0]) != 14:
                    n = reg(ins.args[0])
                    self.emit(f"{{ u32 r_ = LR; call_addr(R({n})); call_addr(r_); return; }}")
                    prev_ldr_lr = False
                    prev_mov_lr_pc = False
                    continue
                prev_ldr_lr = (ins.op == "ldr" and ins.cond == "al" and len(ins.args) >= 2
                               and ins.args[0].strip() == "lr")
                prev_mov_lr_pc = (ins.op == "mov" and not ins.thumb and len(ins.args) == 2
                                  and ins.args[0].strip() == "lr" and ins.args[1].strip() == "pc")
                if ins.op == "?":
                    raise Untranslatable("mnemonic")
                self.translate(ins, needed[idx])
            except (Untranslatable, ValueError, KeyError, IndexError) as e:
                self.unimpl += 1
                txt = ins.text.replace("\\", "\\\\").replace('"', '\\"')
                self.emit(f'UNIMPL("{self.cname}", "{txt}");  /* {e} */')
        nxt = self.f.get("next")
        if nxt:
            self.emit(f"{nxt}();  /* falls through into the next function */")
        self.out.append("}")
        return self.out


# ---------------------------------------------------------------- driver

def ends_open(f):
    """the function's last instruction lets execution run into the next one"""
    for it in reversed(f["items"]):
        if it[0] == "label":
            continue
        if it[0] == "data":
            if it[1].startswith((".balign", ".align")):
                continue
            return False                 # literal pool
        ins = it[1]
        if ins.op == "nop":
            continue
        a = [x.strip().lower() for x in ins.args]
        if ins.cond == "al":
            if ins.op in ("b", "bx"):
                return False
            if ins.op in ("pop", "ldm") and "pc" in ins.text.lower().split(",", 1)[-1]:
                return False
            if a and a[0] == "pc":
                return False
        return True
    return False


def write_if_changed(path, text):
    """keeps the file (and its object) when the translation did not change"""
    if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
        return
    with open(path, "w", encoding="utf-8", newline="\n") as o:
        o.write(text)


def cname_for(fileid, name):
    pre = "" if fileid in ("main", "itcm", "dtcm", "ltdmain", "middleware") else fileid + "_"
    return pre + re.sub(r"\W", "_", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repo")
    ap.add_argument("out")
    ap.add_argument("--files", nargs="*", default=None, help="asm file ids (default: all)")
    ap.add_argument("--extra", nargs="*", default=["lib/asm/crt0.s"],
                    help="other asm files of the repository, translated as part of main")
    ap.add_argument("--chunk", type=int, default=600)
    ap.add_argument("--version", choices=["black2", "white2"], default="black2")
    ap.add_argument("--addr-map", default=None,
                    help="White 2: real function addresses (tools/recomp/w2addr.py output)")
    a = ap.parse_args()
    CONSTS["GAME_VERSION"] = 23 if a.version == "black2" else 22
    DEFINES.clear()
    DEFINES.add("BLACK2" if a.version == "black2" else "WHITE2")
    global ADDR_MAP
    if a.version == "white2" and not a.addr_map:
        raise SystemExit("--version white2 needs --addr-map (python tools/recomp/w2addr.py ...)")
    ADDR_MAP = json.load(open(a.addr_map)) if a.addr_map else None

    asm_dir = os.path.join(a.repo, "asm")
    ids = a.files or sorted(os.path.splitext(n)[0] for n in os.listdir(asm_dir) if n.endswith(".s"))
    files = {}
    for fid in ids:
        files[fid] = parse_file(os.path.join(asm_dir, fid + ".s"))
    for extra in a.extra:
        files["main"] = files.get("main", []) + parse_file(os.path.join(a.repo, extra))

    for fid, fs in files.items():
        labs = FuncGen.file_labels.setdefault(fid, {})
        for f in fs:
            for it in f["items"]:
                if it[0] == "label" and it[2] is not None:
                    labs.setdefault(it[1], it[2])
        for i, f in enumerate(fs):
            FuncGen.func_names.add(f["name"])
            FuncGen.inner_labels.update(it[1] for it in f["items"] if it[0] == "label" and it[1] != f["name"])
            if i and f["thumb"] and fs[i - 1]["thumb"] and ends_open(fs[i - 1]):
                first = next((it[1] for it in f["items"] if it[0] == "insn"), None)
                if first is not None and first.op not in ("push", "stm"):
                    FuncGen.continuations.add(f["name"])
    FuncGen.inner_labels -= FuncGen.func_names
    print("split-off continuations:", " ".join(sorted(FuncGen.continuations)))

    glob_names = {}
    for fid in ("main", "itcm", "dtcm", "ltdmain", "middleware"):
        for f in files.get(fid, []):
            glob_names[f["name"]] = cname_for(fid, f["name"])
    local = {fid: {f["name"]: cname_for(fid, f["name"]) for f in fs} for fid, fs in files.items()}
    # a function of another overlay named in the asm (OV139_FUN_...): when the
    # name exists in one file only, that is the function meant (a direct call;
    # call_addr() with the name's address would lose the Thumb bit and could
    # pick another overlay's ARM code at the same address)
    owners = {}
    for fid, fs in files.items():
        for f in fs:
            owners.setdefault(f["name"], []).append(fid)
    unique = {n: cname_for(fids[0], n) for n, fids in owners.items() if len(fids) == 1}

    def resolve(name, fileid):
        if name in SVC:
            return f"swi_call(0x{SVC[name]:X})"
        if name.startswith("DSProt_"):
            return f"dsprot_call(\"{name}\", 0, 0)"     # anti-piracy checks: runtime answers "genuine"
        if name in local.get(fileid, {}):
            return local[fileid][name] + "()"
        if name in glob_names:
            return glob_names[name] + "()"
        if name in unique:
            return unique[name] + "()"
        if ADDR_MAP:                      # White 2: the measured address of that name
            for mod in ADDR_MAP.values():
                if isinstance(mod, dict) and name in mod:
                    return f"call_addr(0x{mod[name]:08X}u)"
        m = re.search(r"([0-9A-Fa-f]{8})$", name)
        if m:
            return f"call_addr(0x{m.group(1)}u)"
        raise Untranslatable("symbol " + name)

    # labels branched to from another function: secondary entry points
    BR = re.compile(r"^b(l|lx)?(eq|ne|hs|cs|lo|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.n|\.w)?$")
    entries = {}                       # fid -> {label: (func, addr)}
    for fid, fs in files.items():
        owner = {}
        for f in fs:
            for it in f["items"]:
                if it[0] == "label" and it[2] is not None and it[1] != f["name"]:
                    owner[it[1]] = (f, it[2])
        for f in fs:
            for it in f["items"]:
                if it[0] != "insn" or it[1] is None or not BR.match(it[1].op or ""):
                    continue
                if not it[1].args:
                    continue
                tgt = it[1].args[0].strip()
                if tgt in owner and owner[tgt][0] is not f:
                    entries.setdefault(fid, {})[tgt] = owner[tgt]
    for fid, es in entries.items():
        for lab in es:
            cn = cname_for(fid, es[lab][0]["name"]) + "__" + re.sub(r"\W", "_", lab)
            local.setdefault(fid, {})[lab] = cn
            if fid in ("main", "itcm", "dtcm", "ltdmain", "middleware"):
                glob_names[lab] = cn

    os.makedirs(a.out, exist_ok=True)
    total_ins = total_unimpl = 0
    reasons = defaultdict(int)
    dispatch = []
    decls = []
    for fid, fs in files.items():
        for i, f in enumerate(fs):
            f["next"] = cname_for(fid, fs[i + 1]["name"]) if i + 1 < len(fs) else None
        chunks = [fs[i:i + a.chunk] for i in range(0, len(fs), a.chunk)]
        for ci, chunk in enumerate(chunks):
            lines = ['#include "cpu.h"', '#include "rc_decls.h"', ""]
            for f in chunk:
                cn = cname_for(fid, f["name"])
                gen = FuncGen(f, cn, resolve, fid)
                body = gen.generate()
                # functions ending in a fall-through are rare; keep the call only
                # when the last instruction is not a return/branch
                lines.extend(body)
                lines.append("")
                total_ins += sum(1 for it in f["items"] if it[0] == "insn")
                total_unimpl += gen.unimpl
                for l in body:
                    m = re.search(r"/\* (.+?) \*/$", l)
                    if "UNIMPL(" in l and m:
                        reasons[m.group(1).split(" ")[0]] += 1
                decls.append(f"void {cn}(void);")
                if f["addr"] is not None:
                    dispatch.append((f["addr"] | (1 if f["thumb"] else 0), fid, cn))
                for lab, (ef, eaddr) in entries.get(fid, {}).items():
                    if ef is not f or f["name"] in HLE:
                        continue
                    ecn = local[fid][lab]
                    eg = FuncGen(f, ecn, resolve, fid)
                    eg.entry_label = lab
                    lines.extend(eg.generate())
                    lines.append("")
                    decls.append(f"void {ecn}(void);")
                    dispatch.append((eaddr | (1 if f["thumb"] else 0), fid, ecn))
            write_if_changed(os.path.join(a.out, f"rc_{fid}_{ci}.c"), "\n".join(lines) + "\n")
    hle_decls = [f"void hle_{n}(void);" for n in HLE] + [f"void {h}(void);" for h in sorted(set(HOOKS.values()))]
    write_if_changed(os.path.join(a.out, "rc_decls.h"),
                     "/* generated by tools/recomp/recomp.py */\n#pragma once\n"
                     + "\n".join(sorted(set(decls)) + hle_decls) + "\n")
    with open(os.path.join(a.out, "rc_dispatch.c"), "w", encoding="utf-8", newline="\n") as o:
        o.write('#include "cpu.h"\n#include "rc_decls.h"\n#include "dispatch.h"\n\n')
        o.write("const DispatchEntry gDispatch[] = {\n")
        for addr, fid, cn in sorted(dispatch):
            ov = -1 if not fid.startswith("overlay_") else int(fid.split("_")[1])
            o.write(f"    {{ 0x{addr:08X}u, {ov}, {cn} }},\n")
        o.write("};\nconst unsigned gDispatchCount = sizeof gDispatch / sizeof gDispatch[0];\n")
    # main-module addresses the runtime knows (thread info, handshake...): the
    # Black 2 -> this game table, and the shift of the static bss
    entries = []
    bss = [0, 0, 0]
    if ADDR_MAP:
        for name, w in ADDR_MAP.get("main", {}).items():
            mh = re.search(r"([0-9A-Fa-f]{8})$", name)
            if mh:
                entries.append((int(mh.group(1), 16), w))
        bss = ADDR_MAP.get("_bss", bss)
    entries.sort()
    with open(os.path.join(a.out, "rc_addrmap.c"), "w", encoding="utf-8", newline="\n") as o:
        o.write("/* generated by tools/recomp/recomp.py: Black 2 address -> this game's */\n")
        o.write('#include "cpu.h"\n\n')
        o.write("const u32 gAddrMap[][2] = {\n")
        for b, w in entries:
            o.write(f"    {{ 0x{b:08X}u, 0x{w:08X}u }},\n")
        if not entries:
            o.write("    { 0, 0 },\n")
        o.write("};\n")
        o.write(f"const unsigned gAddrMapCount = {len(entries)};\n")
        o.write(f"const u32 gBssStart = 0x{bss[0]:08X}u, gBssEnd = 0x{bss[1]:08X}u;\n")
        o.write(f"const s32 gBssDelta = {bss[2]};\n")
    pct = 100.0 * (total_ins - total_unimpl) / max(1, total_ins)
    print(f"{len(files)} files, {len(decls)} functions, {total_ins} instructions, "
          f"{total_unimpl} untranslated ({pct:.3f}% translated)")
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:20]:
        print(f"  {v:6d}  {k}")


if __name__ == "__main__":
    main()
