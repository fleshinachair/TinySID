#include "cpu6502.h"

enum {
    AM_IMP, AM_ACC, AM_IMM, AM_ZP, AM_ZPX, AM_ZPY, AM_ABS, AM_ABX, AM_ABY,
    AM_IND, AM_IZX, AM_IZY, AM_REL
};

enum {
    ADC, AHX, ALR, ANC, AND, ARR, ASL, AXS, BCC, BCS, BEQ, BIT, BMI, BNE, BPL,
    BRK, BVC, BVS, CLC, CLD, CLI, CLV, CMP, CPX, CPY, DCP, DEC, DEX, DEY, EOR,
    INC, INX, INY, ISC, JMP, JSR, KIL, LAS, LAX, LDA, LDX, LDY, LSR, NOP, ORA,
    PHA, PHP, PLA, PLP, RLA, ROL, ROR, RRA, RTI, RTS, SAX, SBC, SEC, SED, SEI,
    SHX, SHY, SLO, SRE, STA, STX, STY, TAS, TAX, TAY, TSX, TXA, TXS, TYA, XAA
};

typedef struct { uint8_t op, mode, cyc; } opinfo_t;

#define O(op, mode, cyc) { op, mode, cyc }
#define KILL O(KIL, AM_IMP, 2)

static const opinfo_t optab[256] = {
    /* 00 */ O(BRK,AM_IMP,7), O(ORA,AM_IZX,6), KILL, O(SLO,AM_IZX,8),
             O(NOP,AM_ZP,3),  O(ORA,AM_ZP,3),  O(ASL,AM_ZP,5), O(SLO,AM_ZP,5),
             O(PHP,AM_IMP,3), O(ORA,AM_IMM,2), O(ASL,AM_ACC,2), O(ANC,AM_IMM,2),
             O(NOP,AM_ABS,4), O(ORA,AM_ABS,4), O(ASL,AM_ABS,6), O(SLO,AM_ABS,6),
    /* 10 */ O(BPL,AM_REL,2), O(ORA,AM_IZY,5), KILL, O(SLO,AM_IZY,8),
             O(NOP,AM_ZPX,4), O(ORA,AM_ZPX,4), O(ASL,AM_ZPX,6), O(SLO,AM_ZPX,6),
             O(CLC,AM_IMP,2), O(ORA,AM_ABY,4), O(NOP,AM_IMP,2), O(SLO,AM_ABY,7),
             O(NOP,AM_ABX,4), O(ORA,AM_ABX,4), O(ASL,AM_ABX,7), O(SLO,AM_ABX,7),
    /* 20 */ O(JSR,AM_ABS,6), O(AND,AM_IZX,6), KILL, O(RLA,AM_IZX,8),
             O(BIT,AM_ZP,3),  O(AND,AM_ZP,3),  O(ROL,AM_ZP,5), O(RLA,AM_ZP,5),
             O(PLP,AM_IMP,4), O(AND,AM_IMM,2), O(ROL,AM_ACC,2), O(ANC,AM_IMM,2),
             O(BIT,AM_ABS,4), O(AND,AM_ABS,4), O(ROL,AM_ABS,6), O(RLA,AM_ABS,6),
    /* 30 */ O(BMI,AM_REL,2), O(AND,AM_IZY,5), KILL, O(RLA,AM_IZY,8),
             O(NOP,AM_ZPX,4), O(AND,AM_ZPX,4), O(ROL,AM_ZPX,6), O(RLA,AM_ZPX,6),
             O(SEC,AM_IMP,2), O(AND,AM_ABY,4), O(NOP,AM_IMP,2), O(RLA,AM_ABY,7),
             O(NOP,AM_ABX,4), O(AND,AM_ABX,4), O(ROL,AM_ABX,7), O(RLA,AM_ABX,7),
    /* 40 */ O(RTI,AM_IMP,6), O(EOR,AM_IZX,6), KILL, O(SRE,AM_IZX,8),
             O(NOP,AM_ZP,3),  O(EOR,AM_ZP,3),  O(LSR,AM_ZP,5), O(SRE,AM_ZP,5),
             O(PHA,AM_IMP,3), O(EOR,AM_IMM,2), O(LSR,AM_ACC,2), O(ALR,AM_IMM,2),
             O(JMP,AM_ABS,3), O(EOR,AM_ABS,4), O(LSR,AM_ABS,6), O(SRE,AM_ABS,6),
    /* 50 */ O(BVC,AM_REL,2), O(EOR,AM_IZY,5), KILL, O(SRE,AM_IZY,8),
             O(NOP,AM_ZPX,4), O(EOR,AM_ZPX,4), O(LSR,AM_ZPX,6), O(SRE,AM_ZPX,6),
             O(CLI,AM_IMP,2), O(EOR,AM_ABY,4), O(NOP,AM_IMP,2), O(SRE,AM_ABY,7),
             O(NOP,AM_ABX,4), O(EOR,AM_ABX,4), O(LSR,AM_ABX,7), O(SRE,AM_ABX,7),
    /* 60 */ O(RTS,AM_IMP,6), O(ADC,AM_IZX,6), KILL, O(RRA,AM_IZX,8),
             O(NOP,AM_ZP,3),  O(ADC,AM_ZP,3),  O(ROR,AM_ZP,5), O(RRA,AM_ZP,5),
             O(PLA,AM_IMP,4), O(ADC,AM_IMM,2), O(ROR,AM_ACC,2), O(ARR,AM_IMM,2),
             O(JMP,AM_IND,5), O(ADC,AM_ABS,4), O(ROR,AM_ABS,6), O(RRA,AM_ABS,6),
    /* 70 */ O(BVS,AM_REL,2), O(ADC,AM_IZY,5), KILL, O(RRA,AM_IZY,8),
             O(NOP,AM_ZPX,4), O(ADC,AM_ZPX,4), O(ROR,AM_ZPX,6), O(RRA,AM_ZPX,6),
             O(SEI,AM_IMP,2), O(ADC,AM_ABY,4), O(NOP,AM_IMP,2), O(RRA,AM_ABY,7),
             O(NOP,AM_ABX,4), O(ADC,AM_ABX,4), O(ROR,AM_ABX,7), O(RRA,AM_ABX,7),
    /* 80 */ O(NOP,AM_IMM,2), O(STA,AM_IZX,6), O(NOP,AM_IMM,2), O(SAX,AM_IZX,6),
             O(STY,AM_ZP,3),  O(STA,AM_ZP,3),  O(STX,AM_ZP,3), O(SAX,AM_ZP,3),
             O(DEY,AM_IMP,2), O(NOP,AM_IMM,2), O(TXA,AM_IMP,2), O(XAA,AM_IMM,2),
             O(STY,AM_ABS,4), O(STA,AM_ABS,4), O(STX,AM_ABS,4), O(SAX,AM_ABS,4),
    /* 90 */ O(BCC,AM_REL,2), O(STA,AM_IZY,6), KILL, O(AHX,AM_IZY,6),
             O(STY,AM_ZPX,4), O(STA,AM_ZPX,4), O(STX,AM_ZPY,4), O(SAX,AM_ZPY,4),
             O(TYA,AM_IMP,2), O(STA,AM_ABY,5), O(TXS,AM_IMP,2), O(TAS,AM_ABY,5),
             O(SHY,AM_ABX,5), O(STA,AM_ABX,5), O(SHX,AM_ABY,5), O(AHX,AM_ABY,5),
    /* A0 */ O(LDY,AM_IMM,2), O(LDA,AM_IZX,6), O(LDX,AM_IMM,2), O(LAX,AM_IZX,6),
             O(LDY,AM_ZP,3),  O(LDA,AM_ZP,3),  O(LDX,AM_ZP,3), O(LAX,AM_ZP,3),
             O(TAY,AM_IMP,2), O(LDA,AM_IMM,2), O(TAX,AM_IMP,2), O(LAX,AM_IMM,2),
             O(LDY,AM_ABS,4), O(LDA,AM_ABS,4), O(LDX,AM_ABS,4), O(LAX,AM_ABS,4),
    /* B0 */ O(BCS,AM_REL,2), O(LDA,AM_IZY,5), KILL, O(LAX,AM_IZY,5),
             O(LDY,AM_ZPX,4), O(LDA,AM_ZPX,4), O(LDX,AM_ZPY,4), O(LAX,AM_ZPY,4),
             O(CLV,AM_IMP,2), O(LDA,AM_ABY,4), O(TSX,AM_IMP,2), O(LAS,AM_ABY,4),
             O(LDY,AM_ABX,4), O(LDA,AM_ABX,4), O(LDX,AM_ABY,4), O(LAX,AM_ABY,4),
    /* C0 */ O(CPY,AM_IMM,2), O(CMP,AM_IZX,6), O(NOP,AM_IMM,2), O(DCP,AM_IZX,8),
             O(CPY,AM_ZP,3),  O(CMP,AM_ZP,3),  O(DEC,AM_ZP,5), O(DCP,AM_ZP,5),
             O(INY,AM_IMP,2), O(CMP,AM_IMM,2), O(DEX,AM_IMP,2), O(AXS,AM_IMM,2),
             O(CPY,AM_ABS,4), O(CMP,AM_ABS,4), O(DEC,AM_ABS,6), O(DCP,AM_ABS,6),
    /* D0 */ O(BNE,AM_REL,2), O(CMP,AM_IZY,5), KILL, O(DCP,AM_IZY,8),
             O(NOP,AM_ZPX,4), O(CMP,AM_ZPX,4), O(DEC,AM_ZPX,6), O(DCP,AM_ZPX,6),
             O(CLD,AM_IMP,2), O(CMP,AM_ABY,4), O(NOP,AM_IMP,2), O(DCP,AM_ABY,7),
             O(NOP,AM_ABX,4), O(CMP,AM_ABX,4), O(DEC,AM_ABX,7), O(DCP,AM_ABX,7),
    /* E0 */ O(CPX,AM_IMM,2), O(SBC,AM_IZX,6), O(NOP,AM_IMM,2), O(ISC,AM_IZX,8),
             O(CPX,AM_ZP,3),  O(SBC,AM_ZP,3),  O(INC,AM_ZP,5), O(ISC,AM_ZP,5),
             O(INX,AM_IMP,2), O(SBC,AM_IMM,2), O(NOP,AM_IMP,2), O(SBC,AM_IMM,2),
             O(CPX,AM_ABS,4), O(SBC,AM_ABS,4), O(INC,AM_ABS,6), O(ISC,AM_ABS,6),
    /* F0 */ O(BEQ,AM_REL,2), O(SBC,AM_IZY,5), KILL, O(ISC,AM_IZY,8),
             O(NOP,AM_ZPX,4), O(SBC,AM_ZPX,4), O(INC,AM_ZPX,6), O(ISC,AM_ZPX,6),
             O(SED,AM_IMP,2), O(SBC,AM_ABY,4), O(NOP,AM_IMP,2), O(ISC,AM_ABY,7),
             O(NOP,AM_ABX,4), O(SBC,AM_ABX,4), O(INC,AM_ABX,7), O(ISC,AM_ABX,7),
};

static inline uint8_t rd(cpu6502_t *c, uint16_t a) { return c->read(c->ctx, a); }
static inline void wr(cpu6502_t *c, uint16_t a, uint8_t v) { c->write(c->ctx, a, v); }

static inline uint8_t fetch8(cpu6502_t *c) { return rd(c, c->pc++); }
static inline uint16_t fetch16(cpu6502_t *c)
{
    uint16_t lo = fetch8(c);
    return (uint16_t)(lo | (fetch8(c) << 8));
}

void cpu_push(cpu6502_t *c, uint8_t v) { wr(c, (uint16_t)(0x100 | c->sp), v); c->sp--; }
static inline uint8_t pull(cpu6502_t *c) { c->sp++; return rd(c, (uint16_t)(0x100 | c->sp)); }

static inline void set_flag(cpu6502_t *c, uint8_t f, int on)
{
    if (on) c->p |= f; else c->p &= (uint8_t)~f;
}

static inline uint8_t set_nz(cpu6502_t *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(CPU_N | CPU_Z)) | (v & CPU_N) | (v == 0 ? CPU_Z : 0));
    return v;
}

void cpu_reset(cpu6502_t *c)
{
    c->a = c->x = c->y = 0;
    c->sp = 0xFD;
    c->p = CPU_I | CPU_U;
    c->pc = 0;
    c->jammed = 0;
    c->cycles = 0;
}

static void do_adc(cpu6502_t *c, uint8_t m)
{
    unsigned a = c->a, carry = c->p & CPU_C;
    unsigned bin = a + m + carry;
    if (c->p & CPU_D) {
        unsigned al = (a & 0x0F) + (m & 0x0F) + carry;
        if (al >= 0x0A) al = ((al + 6) & 0x0F) + 0x10;
        unsigned sum = (a & 0xF0) + (m & 0xF0) + al;
        set_flag(c, CPU_N, sum & 0x80);
        set_flag(c, CPU_V, (~(a ^ m) & (a ^ sum)) & 0x80);
        if (sum >= 0xA0) sum += 0x60;
        set_flag(c, CPU_C, sum >= 0x100);
        set_flag(c, CPU_Z, (bin & 0xFF) == 0);
        c->a = (uint8_t)sum;
    } else {
        set_flag(c, CPU_C, bin > 0xFF);
        set_flag(c, CPU_V, (~(a ^ m) & (a ^ bin)) & 0x80);
        c->a = set_nz(c, (uint8_t)bin);
    }
}

static void do_sbc(cpu6502_t *c, uint8_t m)
{
    unsigned a = c->a, carry = c->p & CPU_C;
    unsigned bin = a - m - (1 - carry);
    set_flag(c, CPU_V, ((a ^ m) & (a ^ bin)) & 0x80);
    set_flag(c, CPU_C, !(bin & 0x100));
    if (c->p & CPU_D) {
        int al = (int)(a & 0x0F) - (int)(m & 0x0F) + (int)carry - 1;
        if (al < 0) al = ((al - 6) & 0x0F) - 0x10;
        int r = (int)(a & 0xF0) - (int)(m & 0xF0) + al;
        if (r < 0) r -= 0x60;
        set_nz(c, (uint8_t)bin);
        c->a = (uint8_t)r;
    } else {
        c->a = set_nz(c, (uint8_t)bin);
    }
}

static void do_cmp(cpu6502_t *c, uint8_t r, uint8_t m)
{
    set_flag(c, CPU_C, r >= m);
    set_nz(c, (uint8_t)(r - m));
}

static uint8_t do_asl(cpu6502_t *c, uint8_t v) { set_flag(c, CPU_C, v & 0x80); return set_nz(c, (uint8_t)(v << 1)); }
static uint8_t do_lsr(cpu6502_t *c, uint8_t v) { set_flag(c, CPU_C, v & 1); return set_nz(c, (uint8_t)(v >> 1)); }
static uint8_t do_rol(cpu6502_t *c, uint8_t v)
{
    uint8_t cin = c->p & CPU_C;
    set_flag(c, CPU_C, v & 0x80);
    return set_nz(c, (uint8_t)((v << 1) | cin));
}
static uint8_t do_ror(cpu6502_t *c, uint8_t v)
{
    uint8_t cin = (uint8_t)((c->p & CPU_C) << 7);
    set_flag(c, CPU_C, v & 1);
    return set_nz(c, (uint8_t)((v >> 1) | cin));
}

static int reads_with_penalty(int op)
{
    switch (op) {
    case ORA: case AND: case EOR: case ADC: case SBC: case CMP:
    case LDA: case LDX: case LDY: case LAX: case NOP: case LAS:
        return 1;
    default:
        return 0;
    }
}

int cpu_step(cpu6502_t *c)
{
    if (c->jammed) { c->cycles += 2; return 2; }

    const opinfo_t *o = &optab[fetch8(c)];
    int cyc = o->cyc;
    uint16_t ea = 0, base;
    int crossed = 0;

    switch (o->mode) {
    case AM_IMP: case AM_ACC: break;
    case AM_IMM: ea = c->pc++; break;
    case AM_ZP:  ea = fetch8(c); break;
    case AM_ZPX: ea = (uint8_t)(fetch8(c) + c->x); break;
    case AM_ZPY: ea = (uint8_t)(fetch8(c) + c->y); break;
    case AM_ABS: ea = fetch16(c); break;
    case AM_ABX: base = fetch16(c); ea = (uint16_t)(base + c->x); crossed = (base ^ ea) & 0xFF00; break;
    case AM_ABY: base = fetch16(c); ea = (uint16_t)(base + c->y); crossed = (base ^ ea) & 0xFF00; break;
    case AM_IND: {
        uint16_t ptr = fetch16(c);
        uint16_t hi_addr = (uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0x00FF)); /* page-wrap bug */
        ea = (uint16_t)(rd(c, ptr) | (rd(c, hi_addr) << 8));
        break;
    }
    case AM_IZX: {
        uint8_t zp = (uint8_t)(fetch8(c) + c->x);
        ea = (uint16_t)(rd(c, zp) | (rd(c, (uint8_t)(zp + 1)) << 8));
        break;
    }
    case AM_IZY: {
        uint8_t zp = fetch8(c);
        base = (uint16_t)(rd(c, zp) | (rd(c, (uint8_t)(zp + 1)) << 8));
        ea = (uint16_t)(base + c->y);
        crossed = (base ^ ea) & 0xFF00;
        break;
    }
    case AM_REL: {
        int8_t off = (int8_t)fetch8(c);
        ea = (uint16_t)(c->pc + off);
        break;
    }
    }
    if (crossed && reads_with_penalty(o->op)) cyc++;

    uint8_t m, t;
    switch (o->op) {
    /* loads / stores */
    case LDA: c->a = set_nz(c, rd(c, ea)); break;
    case LDX: c->x = set_nz(c, rd(c, ea)); break;
    case LDY: c->y = set_nz(c, rd(c, ea)); break;
    case LAX: c->a = c->x = set_nz(c, rd(c, ea)); break;
    case LAS: c->a = c->x = c->sp = set_nz(c, (uint8_t)(rd(c, ea) & c->sp)); break;
    case STA: wr(c, ea, c->a); break;
    case STX: wr(c, ea, c->x); break;
    case STY: wr(c, ea, c->y); break;
    case SAX: wr(c, ea, (uint8_t)(c->a & c->x)); break;
    case SHY: wr(c, ea, (uint8_t)(c->y & ((ea >> 8) + 1))); break;
    case SHX: wr(c, ea, (uint8_t)(c->x & ((ea >> 8) + 1))); break;
    case AHX: wr(c, ea, (uint8_t)(c->a & c->x & ((ea >> 8) + 1))); break;
    case TAS: c->sp = (uint8_t)(c->a & c->x); wr(c, ea, (uint8_t)(c->sp & ((ea >> 8) + 1))); break;

    /* transfers / stack */
    case TAX: c->x = set_nz(c, c->a); break;
    case TAY: c->y = set_nz(c, c->a); break;
    case TXA: c->a = set_nz(c, c->x); break;
    case TYA: c->a = set_nz(c, c->y); break;
    case TSX: c->x = set_nz(c, c->sp); break;
    case TXS: c->sp = c->x; break;
    case PHA: cpu_push(c, c->a); break;
    case PHP: cpu_push(c, (uint8_t)(c->p | CPU_B | CPU_U)); break;
    case PLA: c->a = set_nz(c, pull(c)); break;
    case PLP: c->p = (uint8_t)((pull(c) & ~CPU_B) | CPU_U); break;

    /* logic / arithmetic */
    case AND: c->a = set_nz(c, (uint8_t)(c->a & rd(c, ea))); break;
    case ORA: c->a = set_nz(c, (uint8_t)(c->a | rd(c, ea))); break;
    case EOR: c->a = set_nz(c, (uint8_t)(c->a ^ rd(c, ea))); break;
    case ADC: do_adc(c, rd(c, ea)); break;
    case SBC: do_sbc(c, rd(c, ea)); break;
    case CMP: do_cmp(c, c->a, rd(c, ea)); break;
    case CPX: do_cmp(c, c->x, rd(c, ea)); break;
    case CPY: do_cmp(c, c->y, rd(c, ea)); break;
    case BIT:
        m = rd(c, ea);
        set_flag(c, CPU_Z, (c->a & m) == 0);
        c->p = (uint8_t)((c->p & ~(CPU_N | CPU_V)) | (m & (CPU_N | CPU_V)));
        break;

    /* shifts / rotates / inc / dec */
    case ASL: if (o->mode == AM_ACC) c->a = do_asl(c, c->a); else wr(c, ea, do_asl(c, rd(c, ea))); break;
    case LSR: if (o->mode == AM_ACC) c->a = do_lsr(c, c->a); else wr(c, ea, do_lsr(c, rd(c, ea))); break;
    case ROL: if (o->mode == AM_ACC) c->a = do_rol(c, c->a); else wr(c, ea, do_rol(c, rd(c, ea))); break;
    case ROR: if (o->mode == AM_ACC) c->a = do_ror(c, c->a); else wr(c, ea, do_ror(c, rd(c, ea))); break;
    case INC: wr(c, ea, set_nz(c, (uint8_t)(rd(c, ea) + 1))); break;
    case DEC: wr(c, ea, set_nz(c, (uint8_t)(rd(c, ea) - 1))); break;
    case INX: c->x = set_nz(c, (uint8_t)(c->x + 1)); break;
    case INY: c->y = set_nz(c, (uint8_t)(c->y + 1)); break;
    case DEX: c->x = set_nz(c, (uint8_t)(c->x - 1)); break;
    case DEY: c->y = set_nz(c, (uint8_t)(c->y - 1)); break;

    /* illegal read-modify-write combos */
    case SLO: t = do_asl(c, rd(c, ea)); wr(c, ea, t); c->a = set_nz(c, (uint8_t)(c->a | t)); break;
    case RLA: t = do_rol(c, rd(c, ea)); wr(c, ea, t); c->a = set_nz(c, (uint8_t)(c->a & t)); break;
    case SRE: t = do_lsr(c, rd(c, ea)); wr(c, ea, t); c->a = set_nz(c, (uint8_t)(c->a ^ t)); break;
    case RRA: t = do_ror(c, rd(c, ea)); wr(c, ea, t); do_adc(c, t); break;
    case DCP: t = (uint8_t)(rd(c, ea) - 1); wr(c, ea, t); do_cmp(c, c->a, t); break;
    case ISC: t = (uint8_t)(rd(c, ea) + 1); wr(c, ea, t); do_sbc(c, t); break;

    /* illegal immediates */
    case ANC: c->a = set_nz(c, (uint8_t)(c->a & rd(c, ea))); set_flag(c, CPU_C, c->a & 0x80); break;
    case ALR: c->a = do_lsr(c, (uint8_t)(c->a & rd(c, ea))); break;
    case ARR:
        t = (uint8_t)(c->a & rd(c, ea));
        c->a = set_nz(c, (uint8_t)((t >> 1) | ((c->p & CPU_C) << 7)));
        set_flag(c, CPU_C, c->a & 0x40);
        set_flag(c, CPU_V, ((c->a >> 6) ^ (c->a >> 5)) & 1);
        break;
    case AXS:
        t = (uint8_t)(c->a & c->x);
        m = rd(c, ea);
        set_flag(c, CPU_C, t >= m);
        c->x = set_nz(c, (uint8_t)(t - m));
        break;
    case XAA: c->a = set_nz(c, (uint8_t)(c->x & rd(c, ea))); break;

    /* flow control */
    case JMP: c->pc = ea; break;
    case JSR:
        cpu_push(c, (uint8_t)((c->pc - 1) >> 8));
        cpu_push(c, (uint8_t)((c->pc - 1) & 0xFF));
        c->pc = ea;
        break;
    case RTS: {
        uint16_t lo = pull(c);
        c->pc = (uint16_t)((lo | (pull(c) << 8)) + 1);
        break;
    }
    case RTI: {
        c->p = (uint8_t)((pull(c) & ~CPU_B) | CPU_U);
        uint16_t lo = pull(c);
        c->pc = (uint16_t)(lo | (pull(c) << 8));
        break;
    }
    case BRK:
        c->pc++;
        cpu_push(c, (uint8_t)(c->pc >> 8));
        cpu_push(c, (uint8_t)(c->pc & 0xFF));
        cpu_push(c, (uint8_t)(c->p | CPU_B | CPU_U));
        c->p |= CPU_I;
        c->pc = (uint16_t)(rd(c, 0xFFFE) | (rd(c, 0xFFFF) << 8));
        break;

#define BRANCH(cond) \
    if (cond) { cyc += 1 + (((c->pc ^ ea) & 0xFF00) ? 1 : 0); c->pc = ea; } break
    case BCC: BRANCH(!(c->p & CPU_C));
    case BCS: BRANCH(c->p & CPU_C);
    case BNE: BRANCH(!(c->p & CPU_Z));
    case BEQ: BRANCH(c->p & CPU_Z);
    case BPL: BRANCH(!(c->p & CPU_N));
    case BMI: BRANCH(c->p & CPU_N);
    case BVC: BRANCH(!(c->p & CPU_V));
    case BVS: BRANCH(c->p & CPU_V);
#undef BRANCH

    /* flags */
    case CLC: c->p &= (uint8_t)~CPU_C; break;
    case SEC: c->p |= CPU_C; break;
    case CLI: c->p &= (uint8_t)~CPU_I; break;
    case SEI: c->p |= CPU_I; break;
    case CLD: c->p &= (uint8_t)~CPU_D; break;
    case SED: c->p |= CPU_D; break;
    case CLV: c->p &= (uint8_t)~CPU_V; break;

    case NOP: break;
    case KIL: c->jammed = 1; c->pc--; break;
    }

    c->cycles += (uint64_t)cyc;
    return cyc;
}
