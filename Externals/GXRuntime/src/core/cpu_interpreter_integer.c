#include "cpu_interpreter_private.h"

u32 ppc_mfspr(CPUState* cpu, u16 spr, u32 cia) {
    if ((cpu->msr & PPC_MSR_PR) && spr != 1 && spr != 8 && spr != 9 &&
        spr != 268 && spr != 269) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return 0;
    }

    switch (spr) {
    case 1: return cpu->xer;
    case 8: return cpu->lr;
    case 9: return cpu->ctr;
    case 18: return cpu->dsisr;
    case 19: return cpu->dar;
    case 26: return cpu->srr0;
    case 27: return cpu->srr1;
    case 268: return (u32)cpu->timebase;
    case 269: return (u32)(cpu->timebase >> 32);
    case 282: return cpu->ear;
    case 287: return cpu->spr_read ? cpu->spr_read(cpu, spr, cia) : 0x00083214u;
    case 912:
    case 913:
    case 914:
    case 915:
    case 916:
    case 917:
    case 918:
    case 919: return cpu->gqr[spr - 912];
    case 920: return cpu->hid2;
    default: break;
    }

    if (cpu->spr_read)
        return cpu->spr_read(cpu, spr, cia);
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
    return 0;
}

void ppc_mtspr(CPUState* cpu, u16 spr, u32 value, u32 cia) {
    if ((cpu->msr & PPC_MSR_PR) && spr != 1 && spr != 8 && spr != 9) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }

    switch (spr) {
    case 1: cpu->xer = value; return;
    case 8: cpu->lr = value; return;
    case 9: cpu->ctr = value; return;
    case 18: cpu->dsisr = value; return;
    case 19: cpu->dar = value; return;
    case 26: cpu->srr0 = value; return;
    case 27: cpu->srr1 = value; return;
    case 282: cpu->ear = value; return;
    case 284:
        cpu->timebase = (cpu->timebase & 0xFFFFFFFF00000000ull) | value;
        break;
    case 285:
        cpu->timebase = ((u64)value << 32) | (cpu->timebase & 0xFFFFFFFFull);
        break;
    case 912:
    case 913:
    case 914:
    case 915:
    case 916:
    case 917:
    case 918:
    case 919:
        cpu->gqr[spr - 912] = value;
        return;
    case 920:
        cpu->hid2 = value;
        return;
    default:
        break;
    }

    if (cpu->spr_write) {
        cpu->spr_write(cpu, spr, value, cia);
        return;
    }
    if (spr != 284 && spr != 285)
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

static bool wrapped_register_range_contains(u8 first, u32 count, u8 reg) {
    for (u32 i = 0; i < count; i++) {
        if (((first + i) & 31u) == reg)
            return true;
    }
    return false;
}

void ppc_lswx(CPUState* cpu, u8 rD, u8 rA, u8 rB, u32 cia) {
    const u32 count = cpu->xer & 0x7Fu;
    const u32 reg_count = (count + 3u) / 4u;
    if (wrapped_register_range_contains(rD, reg_count, rA) ||
        wrapped_register_range_contains(rD, reg_count, rB)) {
        ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
        return;
    }

    const u32 ea = (rA ? cpu->gpr[rA] : 0u) + cpu->gpr[rB];
    for (u32 n = 0; n < count; n++) {
        const u32 reg = (rD + n / 4u) & 31u;
        if ((n & 3u) == 0)
            cpu->gpr[reg] = 0;
        cpu->gpr[reg] |= (u32)mem_read8(cpu, ea + n) << (24u - 8u * (n & 3u));
    }
}

void ppc_cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia) {
    if (operation == PPC_CACHE_DCBI && (cpu->msr & PPC_MSR_PR)) {
        ppc_program_exception(cpu, PPC_PROGRAM_PRIV, cia);
        return;
    }
    if (cpu->cache_control)
        cpu->cache_control(cpu, operation, ea, cia);
}

bool ppc_lwarx_op(CPUState* cpu, u8 d, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return false;
    }
    cpu->gpr[d] = mem_read32(cpu, ea);
    cpu->reserve_valid = true;
    cpu->reserve_addr = ea;
    return true;
}

void ppc_stwcx_op(CPUState* cpu, u8 s, u32 ea, u32 cia) {
    if ((ea & 3u) != 0) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }
    u32 so_bit = (cpu->xer >> 31) & 1u; /* XER.SO -> CR0[SO] */
    if (cpu->reserve_valid && ea == cpu->reserve_addr) {
        mem_write32(cpu, ea, cpu->gpr[s]);
        cpu->reserve_valid = false;
        cpu->cr = (cpu->cr & 0x0FFFFFFFu) | ((2u | so_bit) << 28);
        return;
    }
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (so_bit << 28);
}

void ppc_stsw(CPUState* cpu, u32 ea, u32 n, u8 r, u32 cia) {
    if (cpu->msr & PPC_MSR_LE) {
        ppc_alignment_exception(cpu, ea, cia);
        return;
    }
    if (n == 0)
        return;

    u32 misalignment_bytes = ea & 3u;
    u32 misalignment_bits = misalignment_bytes * 8u;
    u32 current_address = ea & ~3u;
    u64 current_value = 0;

    if (misalignment_bytes != 0) {
        current_value = mem_read32(cpu, current_address);
        current_value <<= misalignment_bits;
        current_value &= 0xFFFFFFFF00000000ull;
        n += misalignment_bytes;
    }

    while (n >= 4) {
        current_value |= cpu->gpr[r];
        mem_write32(cpu, current_address, (u32)(current_value >> misalignment_bits));
        current_value <<= 32;
        current_address += 4;
        n -= 4;
        r = (u8)((r + 1) & 0x1Fu);
    }

    if (n != 0) {
        if (n > misalignment_bytes) {
            current_value |= cpu->gpr[r];
            current_value <<= (n - misalignment_bytes) * 8u;
        } else {
            current_value >>= (misalignment_bytes - n) * 8u;
        }
        current_value &= 0xFFFFFFFF00000000ull;
        current_value |= ((u64)mem_read32(cpu, current_address) << (n * 8u)) & 0xFFFFFFFFull;
        mem_write32(cpu, current_address, (u32)(current_value >> (n * 8u)));
    }
}
