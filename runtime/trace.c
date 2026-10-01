/* trace.c - execution tracing for analysis builds (see trace.h). */
#include "c64.h"
#include "trace.h"

lj_trace_t *lj_trace;

void trace_exec(uint16_t pc, uint8_t op, uint8_t op1, uint8_t op2, uint8_t len, int rom)
{
    lj_trace_t *t = lj_trace;
    t->instructions++;
    uint8_t f = t->flags[pc];
    if (rom) {
        t->flags[pc] = (uint8_t)(f | TR_EXEC | TR_ROM);
        return;
    }
    if (!(f & TR_EXEC)) {
        t->flags[pc] = (uint8_t)(f | TR_EXEC);
        t->first[pc][0] = op;
        t->first[pc][1] = op1;
        t->first[pc][2] = op2;
    } else {
        if (t->first[pc][0] != op)
            f |= TR_VAR_OP;
        if (len >= 2 && t->first[pc][1] != op1)
            f |= TR_VAR_OP1;
        if (len >= 3 && t->first[pc][2] != op2)
            f |= TR_VAR_OP2;
        t->flags[pc] = f;
    }
    for (uint8_t i = 0; i < len; i++)
        t->in_code[(uint16_t)(pc + i)] = 1;
}

void trace_mark_target(uint16_t pc)
{
    if (lj_trace)
        lj_trace->flags[pc] |= TR_IND_TARGET;
}

void trace_write(uint16_t a)
{
    lj_trace_t *t = lj_trace;
    if (t->writes[a] != 0xFFFFFFFFu)
        t->writes[a]++;
    t->known[a] = 1;
    if (t->in_code[a])
        t->smc[a] = 1;
}

void trace_read(uint16_t a)
{
    lj_trace_t *t = lj_trace;
    if (t->known[a])
        return;
    t->known[a] = 1; /* report each address once */
    if (t->uninit_reads < 256) {
        t->uninit_addr[t->uninit_reads] = a;
        t->uninit_pc[t->uninit_reads] = C.cpu.pc;
    }
    t->uninit_reads++;
}
