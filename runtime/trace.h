/*
 * trace.h - optional execution tracing for analysis builds (LJ_TRACE).
 *
 * The recompiler needs to know which bytes are code, which code bytes the
 * game rewrites at run time, and which entry points are reached through
 * indirect jumps. Host analysis tools build the runtime with -DLJ_TRACE
 * and run the game with the interpreter only; the hooks below record that
 * information. Release builds compile the hooks away.
 */
#ifndef LJ_TRACE_H
#define LJ_TRACE_H

#include <stdint.h>

#define TR_EXEC 0x01       /* an instruction started here */
#define TR_VAR_OP 0x02     /* opcode byte differed between executions */
#define TR_VAR_OP1 0x04    /* first operand byte differed */
#define TR_VAR_OP2 0x08    /* second operand byte differed */
#define TR_IND_TARGET 0x10 /* reached by RTS/RTI/JMP (ind)/interrupt */
#define TR_ROM 0x20        /* executed while the address was ROM */

typedef struct {
    uint8_t flags[65536];
    uint8_t first[65536][3];   /* instruction bytes at first execution */
    uint32_t writes[65536];    /* CPU writes to RAM per address */
    uint8_t in_code[65536];    /* byte belongs to an executed instruction */
    uint8_t smc[65536];        /* written after it was executed as code */
    uint8_t known[65536];      /* initialised before the game started */
    uint32_t uninit_reads;
    uint16_t uninit_addr[256], uninit_pc[256];
    uint64_t instructions;
} lj_trace_t;

extern lj_trace_t *lj_trace;

void trace_exec(uint16_t pc, uint8_t op, uint8_t op1, uint8_t op2, uint8_t len, int rom);
void trace_mark_target(uint16_t pc);
void trace_write(uint16_t a);
void trace_read(uint16_t a);

#ifdef LJ_TRACE
#define TRACE_WRITE(a) do { if (lj_trace) trace_write(a); } while (0)
#define TRACE_READ(a) do { if (lj_trace) trace_read(a); } while (0)
#else
#define TRACE_WRITE(a) do { } while (0)
#define TRACE_READ(a) do { } while (0)
#endif

#endif
