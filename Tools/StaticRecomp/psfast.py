#!/usr/bin/env python3
"""Paired-single fast paths for selected generated functions (DolRecomp --function-chunks output).

Usage: Tools/StaticRecomp/psfast.py --generated DIR ADDRESS...   (hex guest addresses)
       Tools/StaticRecomp/psfast.py --translate FUNCTION.c OUT.c  (one function, for tests)

For each listed function (a leaf: no calls, no indirect branches), the generated
`void func_X(CPUState* ctx)` becomes `static void func_X_exact`, and a new `func_X` runs a
translated version first, `func_X_fast`, and the exact one when it declines or stops.

The translated version keeps guest registers in locals and runs paired-single arithmetic on NEON
f64x2 (core/cpu_ps_vector.h): each operation computes the exact helper's f64 result and rounds it
to single, which is the exact helper's result whenever that f64 result is finite. Integer
instructions keep the C DolRecomp emitted, on the locals. It runs when entered where the exact
function can resume a call (its start, or a loop head it yields at) with FPRF off, MSR[FP] set (and HID2[LSQE] when a psq instruction is used); otherwise it declines before doing
anything. It yields where the exact function yields (the same cycle budget at the same loop
back-edges) and returns at blr, writing the registers back. An arithmetic result that is not
finite stops it before that instruction, registers written back and pc there (the cycles of that
instruction's block given back when it starts one), and the exact function, called right away and
entered there, executes it with its NaN and infinity rules. So guest state and timing are the
exact function's, bit for bit.

Anything the translator does not know (another instruction, a helper call it cannot see through, a
return it did not expect) leaves the function as DolRecomp generated it.
"""
import argparse
import re
import sys
from pathlib import Path

FP_OPS = {"ps_add", "ps_sub", "ps_mul", "ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub", "ps_merge00",
          "ps_merge01", "ps_merge10", "ps_merge11", "ps_mr", "fmr", "psq_l", "psq_lu", "psq_st",
          "psq_stu", "lfs", "lfd", "stfd"}
# Calls integer instruction bodies may make: none of them touches a register in ctx.
ALLOWED_CALLS = {"mem_read8", "mem_read16", "mem_read32", "mem_read64", "mem_write8", "mem_write16",
                 "mem_write32", "mem_write64", "dolrecomp_rotl32", "dolrecomp_cntlz32",
                 "dolrecomp_f64_to_bits", "dolrecomp_f64_from_bits", "dolrecomp_f32_from_bits",
                 "if", "for", "while", "switch", "sizeof"}
FIELDS = ["cr", "xer", "ctr", "lr"]
BUDGET = re.compile(r"if \(ctx->downcount <= -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET\) \{\s*"
                    r"ctx->pc = (0x[0-9A-F]+)u;\s*return;\s*\}")


class Unsupported(Exception):
    pass


def translate(text):
    m = re.match(r"void (func_([0-9A-F]{8}))\(CPUState\* ctx\) \{", text)
    if not m:
        raise Unsupported("not a generated function")
    name, entry = m.group(1), int(m.group(2), 16)
    blocks = re.split(r"\n(?=label_[0-9A-F]{8}:\n)", text)
    gprs, fprs, gqrs, used_fields = set(), set(), set(), set()
    entries = {entry}
    needs_lsqe = False
    out = []

    def fr(op):
        n = int(op.lstrip("f"))
        fprs.add(n)
        return f"V_{n}"

    def ea_of(op):
        mm = re.fullmatch(r"(-?\d+)\(r(\d+)\)", op)
        if not mm:
            raise Unsupported(f"address operand {op}")
        gprs.add(int(mm.group(2)))
        return f"R_{mm.group(2)} + (u32)(s32)({mm.group(1)})", int(mm.group(2))

    for b in blocks[1:]:
        addr = int(b[6:14], 16)
        if b is blocks[-1]:
            lines = b.rstrip("\n").split("\n")
            if lines[-1] != "}":
                raise Unsupported("no closing brace")
            b = "\n".join(lines[:-1])
        # Yields first (their target is on a pc line the next step drops).
        def budget(mm):
            target = int(mm.group(1), 16)
            entries.add(target)
            return (f"if (dc0 - dc <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) "
                    f"{{ bail_pc = 0x{target:08X}u; goto bail; }}")
        b = BUDGET.sub(budget, b)
        cost, comment, body = 0, None, []
        for line in b.split("\n")[1:]:
            s = line.strip()
            if re.fullmatch(r"ctx->pc = 0x[0-9A-F]+u;", s):
                continue
            mm = re.fullmatch(r"ctx->downcount -= (\d+);", s)
            if mm:
                cost = int(mm.group(1))
                continue
            mm = re.fullmatch(r"// ([0-9A-F]{8}): (\S+)\s*(.*)", s)
            if mm and comment is None:
                comment = (mm.group(2), [o.strip() for o in mm.group(3).split(",")] if mm.group(3) else [])
                continue
            if re.fullmatch(r"if \(!ppc_fp_available_inline\(ctx, 0x[0-9A-F]+u\)\) return;", s):
                continue
            body.append(line)
        if comment is None:
            raise Unsupported(f"block at {addr:08X} is not one instruction (outlined loop?)")
        mnem, ops = comment
        out.append(f"label_{addr:08X}:")
        if cost:
            out.append(f"    dc += {cost};")
        if mnem in FP_OPS:
            # Stop before this instruction: its block's cycles are charged again when the exact
            # function enters here (a block start), so give them back.
            bail = (f"if (__builtin_expect(!gxv_finite(r), 0)) {{ bail_pc = 0x{addr:08X}u; dc -= {cost}; "
                    f"status = 2; goto bail; }}")
            if mnem in ("ps_add", "ps_sub", "ps_mul"):
                raw = {"ps_add": "gxv_add_raw", "ps_sub": "gxv_sub_raw", "ps_mul": "gxv_mul_raw"}[mnem]
                code = f"{{ const gxv r = {raw}({fr(ops[1])}, {fr(ops[2])}); {bail} {fr(ops[0])} = gxv_round(r, ni); }}"
            elif mnem in ("ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub"):
                d, a, c, bb = ops
                sub = "true" if mnem.endswith("sub") else "false"
                rnd = "vnegq_f64(gxv_round(r, ni))" if mnem.startswith("ps_n") else "gxv_round(r, ni)"
                code = (f"{{ const gxv r = gxv_madd_raw({fr(a)}, {fr(c)}, {fr(bb)}, {sub}); {bail} "
                        f"{fr(d)} = {rnd}; }}")
            elif mnem.startswith("ps_merge"):
                code = f"{fr(ops[0])} = gxv_merge{mnem[-2:]}({fr(ops[1])}, {fr(ops[2])});"
            elif mnem == "ps_mr":
                code = f"{fr(ops[0])} = {fr(ops[1])};"
            elif mnem == "fmr":
                code = f"{fr(ops[0])} = vcopyq_laneq_f64({fr(ops[0])}, 0, {fr(ops[1])}, 0);"
            elif mnem in ("psq_l", "psq_lu", "psq_st", "psq_stu"):
                r, mop, w, i = ops
                ea, base = ea_of(mop)
                gqrs.add(int(i))
                needs_lsqe = True  # the D-form psq instructions are not indexed
                wb = f" R_{base} = ea;" if mnem.endswith("u") else ""
                wflag = "true" if w == "1" else "false"
                if mnem.startswith("psq_l"):
                    code = f"{{ const u32 ea = {ea}; {fr(r)} = gxv_psq_l(ctx, G_{i}, ea, {wflag});{wb} }}"
                else:
                    code = f"{{ const u32 ea = {ea}; gxv_psq_st(ctx, G_{i}, ea, {wflag}, {fr(r)});{wb} }}"
            elif mnem == "lfs":
                ea, _ = ea_of(ops[1])
                code = f"{fr(ops[0])} = vdupq_n_f64(dolrecomp_f32_from_bits(mem_read32(ctx, {ea})));"
            elif mnem == "lfd":
                ea, _ = ea_of(ops[1])
                code = (f"{fr(ops[0])} = vsetq_lane_f64(dolrecomp_f64_from_bits(mem_read64(ctx, {ea})), "
                        f"{fr(ops[0])}, 0);")
            else:  # stfd
                ea, _ = ea_of(ops[1])
                code = f"mem_write64(ctx, {ea}, dolrecomp_f64_to_bits(gxv_ps0({fr(ops[0])})));"
            out.append("    " + code)
            continue
        if re.match(r"(ps_|psq|f|lf|stf|mtfs|mffs)", mnem):
            raise Unsupported(f"{mnem} at {addr:08X}")
        text_body = "\n".join(body)
        if mnem in ("blr", "bclr") or mnem.endswith("lr") and mnem.startswith("b") and not mnem.endswith("lrl"):
            text_body = re.sub(r"ctx->pc = target;\s*return;", "goto fast_exit;", text_body)
        for n in re.findall(r"ctx->gpr\[(\d+)\]", text_body):
            gprs.add(int(n))
        text_body = re.sub(r"ctx->gpr\[(\d+)\]", r"R_\1", text_body)
        for fld in FIELDS:
            if re.search(rf"ctx->{fld}\b", text_body):
                used_fields.add(fld)
            text_body = re.sub(rf"ctx->{fld}\b", f"L_{fld}", text_body)
        left = re.findall(r"ctx->[a-z_0-9]+", text_body)
        if left:
            raise Unsupported(f"{mnem} at {addr:08X} uses {sorted(set(left))}")
        if re.search(r"\breturn\b", text_body):
            raise Unsupported(f"{mnem} at {addr:08X} returns")
        calls = set(re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", text_body)) - ALLOWED_CALLS
        if calls:
            raise Unsupported(f"{mnem} at {addr:08X} calls {sorted(calls)}")
        out.append(text_body)

    if not fprs:
        raise Unsupported("no floating point")
    gprs, fprs = sorted(gprs), sorted(fprs)
    fields = sorted(used_fields | {"lr"}, key=FIELDS.index)
    pre = [f"static int {name}_fast(CPUState* ctx) {{",
           f"    if (g_ppc_fprf_enabled || (ctx->msr & PPC_MSR_FP) == 0"
           f"{' ||' if needs_lsqe else ')'}"]
    if needs_lsqe:
        pre.append("        (ctx->hid2 & PPC_HID2_LSQE) == 0)")
    pre.append("        return 0;")
    pre.append("    switch (ctx->pc) {")
    pre += [f"    case 0x{e:08X}u:" for e in sorted(entries)]
    pre += ["        break;", "    default:", "        return 0;", "    }"]
    pre += [f"    u32 R_{r} = ctx->gpr[{r}];" for r in gprs]
    pre += [f"    gxv V_{r} = gxv_make(ctx->fpr[{r}], ctx->ps1[{r}]);" for r in fprs]
    pre += [f"    u32 L_{x} = ctx->{x};" for x in fields]
    pre += [f"    const gxv_gqr G_{i} = gxv_decode(ctx->gqr[{i}]);" for i in sorted(gqrs)]
    pre += ["    const uint64x2_t ni = vdupq_n_u64((ctx->fpscr & 0x4u) != 0 ? ~0ull : 0ull);",
            "    const s64 dc0 = ctx->downcount;", "    s64 dc = 0;", "    u32 bail_pc;", "    int status = 1;",
            "    switch (ctx->pc) {"]
    pre += [f"    case 0x{e:08X}u: goto label_{e:08X};" for e in sorted(entries)]
    pre += ["    default: return 0;", "    }"]
    post = ["fast_exit:", "    bail_pc = L_lr & ~3u;", "bail:"]
    post += [f"    ctx->gpr[{r}] = R_{r};" for r in gprs]
    post += [f"    ctx->fpr[{r}] = gxv_ps0(V_{r});\n    ctx->ps1[{r}] = gxv_ps1(V_{r});" for r in fprs]
    post += [f"    ctx->{x} = L_{x};" for x in fields]
    post += ["    ctx->downcount = dc0 - dc;", "    ctx->pc = bail_pc;", "    return status;", "}"]
    return name, "\n".join(pre + out + post) + "\n", len(blocks) - 1


def patch(generated, addresses):
    chunks = sorted((Path(generated) / "chunks").glob("*.c"))
    for a in addresses:
        name = f"func_{a.upper():0>8}"
        head = f"void {name}(CPUState* ctx) {{"
        path = next((p for p in chunks if head in p.read_text()), None)
        if path is None:
            print(f"psfast: {name}: not found", file=sys.stderr)
            continue
        src = path.read_text()
        begin, end_mark = f"/* psfast.py: {name} */\n", f"/* psfast.py: end {name} */\n"
        if begin in src:
            # Translated before: put the generated function back, then translate it again.
            a, rest = src.split(begin, 1)
            src = a + rest.split(end_mark, 1)[1]
            src = src.replace(f"static void {name}_exact(CPUState* ctx) {{", head, 1)
        start = src.index(head)
        end = src.index("\n}\n", start) + 3
        try:
            _, fast, count = translate(src[start:end])
        except Unsupported as error:
            print(f"psfast: {name}: left as generated ({error})")
            continue
        src = src[:start] + f"static void {name}_exact(CPUState* ctx) {{" + src[start + len(head):]
        src += (begin + "#if defined(__aarch64__)\n#include \"core/cpu_ps_vector.h\"\n" + fast + "#endif\n"
                f"void {name}(CPUState* ctx) {{\n#if defined(__aarch64__)\n"
                f"    /* 0: declined (nothing done); 1: returned or yielded; 2: stopped before an\n"
                f"       instruction the exact function must execute (pc there). */\n"
                f"    if ({name}_fast(ctx) == 1)\n        return;\n#endif\n    {name}_exact(ctx);\n}}\n" + end_mark)
        path.write_text(src)
        print(f"psfast: {name}: {count} instructions translated ({path.name})")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--generated", help="DolRecomp's generated directory")
    parser.add_argument("--translate", nargs=2, metavar=("FUNCTION_C", "OUT_C"))
    parser.add_argument("addresses", nargs="*")
    args = parser.parse_args()
    if args.translate:
        name, fast, count = translate(Path(args.translate[0]).read_text())
        Path(args.translate[1]).write_text(fast)
        print(f"{name}: {count} instructions")
    elif args.generated:
        patch(args.generated, args.addresses)
    else:
        parser.error("--generated or --translate")


if __name__ == "__main__":
    main()
