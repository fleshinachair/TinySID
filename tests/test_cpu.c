/* 6502 core: flags, addressing modes, cycles, stack, illegal opcodes. */
#include "cpu6502.h"
#include "test_util.h"

static uint8_t mem[65536];
static cpu6502_t cpu;

static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }

/* Clears memory and points the BRK vector at $3000. Call before poking test data. */
static void fresh(void)
{
    memset(mem, 0, sizeof(mem));
    mem[0xFFFE] = 0x00;
    mem[0xFFFF] = 0x30;
}

/* Copies the program in, resets the CPU to it and executes exactly `steps` instructions. */
static void run(const uint8_t *prog, size_t len, int steps)
{
    memcpy(mem + 0x200, prog, len);
    cpu.read = rd; cpu.write = wr; cpu.ctx = NULL;
    cpu_reset(&cpu);
    cpu.pc = 0x200;
    for (int i = 0; i < steps; i++) cpu_step(&cpu);
}

#define RUN(steps, ...) do { const uint8_t p_[] = { __VA_ARGS__ }; run(p_, sizeof(p_), steps); } while (0)

static void test_arithmetic(void)
{
    SECTION("adc / sbc / compare");
    fresh(); RUN(2, 0xA9, 0x7F, 0x69, 0x01);               /* LDA #$7F ; ADC #$01 */
    CHECK_EQ(cpu.a, 0x80);
    CHECK(cpu.p & CPU_V); CHECK(cpu.p & CPU_N); CHECK(!(cpu.p & CPU_C)); CHECK(!(cpu.p & CPU_Z));

    fresh(); RUN(3, 0x38, 0xA9, 0x50, 0xE9, 0xB0);         /* SEC ; LDA #$50 ; SBC #$B0 */
    CHECK_EQ(cpu.a, 0xA0);
    CHECK(cpu.p & CPU_V); CHECK(!(cpu.p & CPU_C));

    fresh(); RUN(4, 0xF8, 0x18, 0xA9, 0x19, 0x69, 0x01);   /* SED CLC LDA #$19 ADC #$01 -> BCD $20 */
    CHECK_EQ(cpu.a, 0x20);
    fresh(); RUN(4, 0xF8, 0x38, 0xA9, 0x20, 0xE9, 0x01);   /* SED SEC LDA #$20 SBC #$01 -> BCD $19 */
    CHECK_EQ(cpu.a, 0x19);
    fresh(); RUN(4, 0xF8, 0x18, 0xA9, 0x99, 0x69, 0x01);   /* 99 + 1 -> 00 carry */
    CHECK_EQ(cpu.a, 0x00); CHECK(cpu.p & CPU_C);

    fresh(); RUN(2, 0xA9, 0x40, 0xC9, 0x40);               /* CMP equal */
    CHECK(cpu.p & CPU_Z); CHECK(cpu.p & CPU_C);
    fresh(); RUN(2, 0xA9, 0x10, 0xC9, 0x40);               /* CMP less */
    CHECK(!(cpu.p & CPU_C)); CHECK(cpu.p & CPU_N);
}

static void test_shifts_bit(void)
{
    SECTION("shifts / bit");
    fresh(); RUN(3, 0x38, 0xA9, 0x81, 0x6A);               /* SEC ; LDA #$81 ; ROR A */
    CHECK_EQ(cpu.a, 0xC0); CHECK(cpu.p & CPU_C);
    fresh(); RUN(2, 0xA9, 0x81, 0x0A);                     /* LDA #$81 ; ASL A */
    CHECK_EQ(cpu.a, 0x02); CHECK(cpu.p & CPU_C);

    fresh(); mem[0x10] = 0xC0;
    RUN(2, 0xA9, 0x01, 0x24, 0x10);                        /* LDA #1 ; BIT $10 */
    CHECK(cpu.p & CPU_Z); CHECK(cpu.p & CPU_N); CHECK(cpu.p & CPU_V);
}

static void test_addressing(void)
{
    SECTION("addressing modes");
    fresh();
    mem[0x10] = 0x00; mem[0x11] = 0x20; mem[0x2005] = 0x99;
    RUN(2, 0xA0, 0x05, 0xB1, 0x10);                        /* LDY #5 ; LDA ($10),Y */
    CHECK_EQ(cpu.a, 0x99);

    fresh();
    mem[0x14] = 0x00; mem[0x15] = 0x30; mem[0x3000] = 0x5A;
    RUN(2, 0xA2, 0x04, 0xA1, 0x10);                        /* LDX #4 ; LDA ($10,X) */
    CHECK_EQ(cpu.a, 0x5A);

    fresh(); mem[0x08] = 0x77;
    RUN(2, 0xA2, 0x10, 0xB5, 0xF8);                        /* LDX #$10 ; LDA $F8,X wraps in page 0 */
    CHECK_EQ(cpu.a, 0x77);

    fresh();
    mem[0x10FF] = 0x34; mem[0x1000] = 0x12; mem[0x1100] = 0xEE;
    RUN(1, 0x6C, 0xFF, 0x10);                              /* JMP ($10FF): high byte comes from $1000 */
    CHECK_EQ(cpu.pc, 0x1234);
}

static void test_cycles(void)
{
    SECTION("cycle counts");
    fresh();
    const uint8_t loop[] = { 0xA2, 0x05, 0xCA, 0xD0, 0xFD };   /* LDX #5 ; loop: DEX ; BNE loop */
    memcpy(mem + 0x200, loop, sizeof(loop));
    cpu.read = rd; cpu.write = wr;
    cpu_reset(&cpu); cpu.pc = 0x200;
    while (cpu.pc != 0x205) cpu_step(&cpu);
    CHECK_EQ(cpu.x, 0);
    CHECK_EQ(cpu.cycles, 26);                              /* 2 + 5*2 + 4*3 + 2 */

    fresh();
    const uint8_t px[] = { 0xA2, 0x01, 0xBD, 0xFF, 0x20, 0xBD, 0x00, 0x20 };
    memcpy(mem + 0x200, px, sizeof(px));
    cpu_reset(&cpu); cpu.pc = 0x200;
    cpu_step(&cpu);
    CHECK_EQ(cpu_step(&cpu), 5);                           /* LDA $20FF,X crosses a page */
    CHECK_EQ(cpu_step(&cpu), 4);                           /* LDA $2000,X does not */

    fresh();
    mem[0x200] = 0x20; mem[0x201] = 0x00; mem[0x202] = 0x03;
    cpu_reset(&cpu); cpu.pc = 0x200;
    CHECK_EQ(cpu_step(&cpu), 6);                           /* JSR */
}

static void test_stack_and_calls(void)
{
    SECTION("stack / jsr / brk");
    fresh();
    mem[0x210] = 0xA9; mem[0x211] = 0x42; mem[0x212] = 0x60;   /* sub: LDA #$42 ; RTS */
    cpu.read = rd; cpu.write = wr;
    cpu_reset(&cpu);
    uint8_t sp0 = cpu.sp;
    RUN(3, 0x20, 0x10, 0x02, 0xEA);                        /* JSR $0210 ; NOP */
    CHECK_EQ(cpu.a, 0x42);
    CHECK_EQ(cpu.pc, 0x203);
    CHECK_EQ(cpu.sp, sp0);

    fresh();                                               /* BRK -> RTI resumes after the padding byte */
    mem[0x3000] = 0x40;
    RUN(1, 0x00, 0xFF, 0xA9, 0x07);
    CHECK_EQ(cpu.pc, 0x3000);
    CHECK(cpu.p & CPU_I);
    CHECK(mem[0x100 + cpu.sp + 1] & CPU_B);                /* pushed status has B set */
    cpu_step(&cpu);
    CHECK_EQ(cpu.pc, 0x202);
    cpu_step(&cpu);
    CHECK_EQ(cpu.a, 0x07);

    fresh(); RUN(4, 0xA9, 0x33, 0x48, 0xA9, 0x00, 0x68);   /* LDA ; PHA ; LDA ; PLA */
    CHECK_EQ(cpu.a, 0x33);

    fresh(); RUN(3, 0xA2, 0x00, 0x9A, 0x48);               /* LDX #0 ; TXS ; PHA: sp wraps to $FF */
    CHECK_EQ(cpu.sp, 0xFF);
}

static void test_illegal(void)
{
    SECTION("illegal opcodes");
    fresh(); mem[0x10] = 0x81;
    RUN(2, 0xA9, 0x00, 0x07, 0x10);                        /* LDA #0 ; SLO $10 */
    CHECK_EQ(mem[0x10], 0x02); CHECK_EQ(cpu.a, 0x02); CHECK(cpu.p & CPU_C);

    fresh(); mem[0x10] = 0x55;
    RUN(1, 0xA7, 0x10);                                    /* LAX $10 */
    CHECK_EQ(cpu.a, 0x55); CHECK_EQ(cpu.x, 0x55);

    fresh();
    RUN(3, 0xA9, 0xF0, 0xA2, 0x3C, 0x87, 0x10);            /* LDA #$F0 ; LDX #$3C ; SAX $10 */
    CHECK_EQ(mem[0x10], 0x30);

    fresh();
    RUN(2, 0xA9, 0x05, 0xC7, 0x10);                        /* DCP $10: mem 0 -> $FF, then CMP */
    CHECK_EQ(mem[0x10], 0xFF);

    fresh(); mem[0x10] = 0x00;
    RUN(3, 0x38, 0xA9, 0x20, 0xE7, 0x10);                  /* SEC ; LDA #$20 ; ISC $10: mem 0 -> 1, A = $20 - 1 */
    CHECK_EQ(mem[0x10], 0x01);
    CHECK_EQ(cpu.a, 0x1F);

    fresh();
    RUN(2, 0xA9, 0xFF, 0x4B, 0x0E);                        /* ALR #$0E : (FF & 0E) >> 1 */
    CHECK_EQ(cpu.a, 0x07);

    fresh();
    RUN(3, 0x02);                                          /* KIL jams the core */
    CHECK(cpu.jammed);
    CHECK_EQ(cpu.pc, 0x200);

    fresh();                                               /* NOP variants consume their operands */
    RUN(4, 0x1A, 0x80, 0xFF, 0x0C, 0x34, 0x12, 0xEA);
    CHECK_EQ(cpu.pc, 0x207);
}

int main(void)
{
    test_arithmetic();
    test_shifts_bit();
    test_addressing();
    test_cycles();
    test_stack_and_calls();
    test_illegal();
    return test_finish("test_cpu");
}
