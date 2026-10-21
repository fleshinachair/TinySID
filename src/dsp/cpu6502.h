/* Compact NMOS 6502 core: all official opcodes plus the illegal opcodes that
 * SID tunes commonly use. Memory is reached through read/write callbacks so
 * the host can map SID/CIA registers. Cycle counts include page-cross and
 * branch penalties; bus-level quirks (dummy reads/writes) are not modelled. */
#ifndef CPU6502_H
#define CPU6502_H

#include <stdint.h>

enum {
    CPU_C = 0x01, CPU_Z = 0x02, CPU_I = 0x04, CPU_D = 0x08,
    CPU_B = 0x10, CPU_U = 0x20, CPU_V = 0x40, CPU_N = 0x80
};

typedef struct cpu6502 {
    uint16_t pc;
    uint8_t a, x, y, sp, p;
    int jammed;          /* set by KIL opcodes; cpu_step then just burns cycles */
    uint64_t cycles;     /* total cycles executed since cpu_reset */
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t value);
    void *ctx;
} cpu6502_t;

/* Clears registers (sp=$FD, p=I|U) and the cycle counter; keeps callbacks. */
void cpu_reset(cpu6502_t *c);

/* Executes one instruction and returns the cycles it took. */
int cpu_step(cpu6502_t *c);

void cpu_push(cpu6502_t *c, uint8_t v);

#endif
