/* Original synthetic SH-2 programs; no game or BIOS content. */
#include "core32x/sh2/sh2_interpreter.h"
#ifdef NDEBUG
#undef NDEBUG /* These executable checks must run in Release builds too. */
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    uint8_t memory[0x2000];
    unsigned reads[3], writes[3], notifications, acknowledgements, fetches;
    uint64_t reported_cycles;
    bool clear_irq, stop_on_write, nested;
    uint32_t wait, raise_at;
    KegaSh2 *peer;
} Fixture;

static void put16(Fixture *f, uint32_t a, uint16_t v) {
    assert(a + 1 < sizeof(f->memory));
    f->memory[a] = (uint8_t)(v >> 8); f->memory[a + 1] = (uint8_t)v;
}
static void put32(Fixture *f, uint32_t a, uint32_t v) {
    put16(f, a, (uint16_t)(v >> 16)); put16(f, a + 2, (uint16_t)v);
}
static uint8_t rb(void *user, KegaSh2 *cpu, uint32_t a) {
    Fixture *f = user; (void)cpu;
    assert(a < sizeof(f->memory)); ++f->reads[0]; return f->memory[a];
}
static uint16_t rw(void *user, KegaSh2 *cpu, uint32_t a) {
    Fixture *f = user;
    assert(a + 1 < sizeof(f->memory)); ++f->reads[1];
    if (f->raise_at && f->raise_at == a) {
        f->raise_at = 0; assert(kega_sh2_set_irq(cpu, 8));
    }
    if (f->nested && a == 0x300) {
        f->nested = false;
        assert(kega_sh2_run(cpu, 1) == 0); /* Same-context recursion rejected. */
        assert(kega_sh2_run(f->peer, 1) == 1);
    }
    return (uint16_t)((uint16_t)f->memory[a] << 8 | f->memory[a + 1]);
}
static uint16_t fetch(void *user, KegaSh2 *cpu, uint32_t a) {
    Fixture *f = user; ++f->fetches; return rw(user, cpu, a);
}
static uint32_t rl(void *user, KegaSh2 *cpu, uint32_t a) {
    Fixture *f = user; (void)cpu;
    assert(a + 3 < sizeof(f->memory)); ++f->reads[2];
    return (uint32_t)f->memory[a] << 24 | (uint32_t)f->memory[a + 1] << 16 |
        (uint32_t)f->memory[a + 2] << 8 | f->memory[a + 3];
}
static void wb(void *user, KegaSh2 *cpu, uint32_t a, uint8_t v) {
    Fixture *f = user;
    assert(a < sizeof(f->memory)); ++f->writes[0]; f->memory[a] = v;
    if (f->wait) assert(kega_sh2_add_wait_cycles(cpu, f->wait));
    if (f->stop_on_write) kega_sh2_end_run(cpu);
}
static void ww(void *user, KegaSh2 *cpu, uint32_t a, uint16_t v) {
    Fixture *f = user; (void)cpu; ++f->writes[1]; put16(f, a, v);
}
static void wl(void *user, KegaSh2 *cpu, uint32_t a, uint32_t v) {
    Fixture *f = user; (void)cpu; ++f->writes[2]; put32(f, a, v);
}
static unsigned ack(void *user, KegaSh2 *cpu, unsigned level) {
    Fixture *f = user; ++f->acknowledgements;
    if (f->clear_irq) assert(kega_sh2_set_irq(cpu, 0));
    return 64 + level / 2;
}
static void elapsed(void *user, KegaSh2 *cpu, uint32_t cycles) {
    Fixture *f = user; ++f->notifications; f->reported_cycles += cycles;
    assert(kega_sh2_cycles(cpu) == f->reported_cycles);
}
static KegaSh2 *create(Fixture *f, bool slave) {
    KegaSh2Host host = {f, rb, rw, rl, wb, ww, wl, ack, elapsed, slave ? NULL : fetch};
    KegaSh2 *cpu = kega_sh2_create(&host, slave);
    assert(cpu && ((uintptr_t)kega_sh2_native(cpu) & 31) == 0);
    put32(f, 0, 0x100); put32(f, 4, 0x1f00);
    assert(kega_sh2_reset(cpu)); return cpu;
}
static KegaSh2State state(KegaSh2 *cpu) {
    KegaSh2State value; assert(kega_sh2_get_state(cpu, &value)); return value;
}
static void start(KegaSh2 *cpu, Fixture *f, uint32_t pc) {
    assert(kega_sh2_reset(cpu)); f->reported_cycles = 0;
    KegaSh2State s = state(cpu); s.pc = pc; s.sr = 0;
    assert(kega_sh2_set_state(cpu, &s));
}
int main(void) {
    Fixture master = {0}, slave = {0};
    KegaSh2 *a = create(&master, false), *b = create(&slave, true);
    kega_sh2_set_peer(a, b); kega_sh2_set_peer(b, a);
    assert(kega_sh2_native(a) != kega_sh2_native(b));
    KegaSh2State s = state(a);
    assert(s.pc == 0x100 && s.r[15] == 0x1f00 && s.sr == 0xf0);
    assert(kega_sh2_run(a, 0) == 0 && kega_sh2_run(a, UINT32_MAX) == 0);
    assert(!kega_sh2_set_irq(a, 16) && !kega_sh2_add_wait_cycles(a, 1));

    /* MOV immediate, all memory widths, sign-extension and isolated registers. */
    const uint16_t ops[] = {0xe080, 0x2100, 0x6210, 0x2101, 0x6311, 0x2102, 0x6412};
    for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) put16(&master, 0x100 + i * 2, ops[i]);
    s.r[1] = 0x500; assert(kega_sh2_set_state(a, &s));
    assert(kega_sh2_run(a, 7) == 7);
    s = state(a);
    assert(s.r[0] == 0xffffff80u && s.r[2] == s.r[0] && s.r[3] == s.r[0] && s.r[4] == s.r[0]);
    assert(master.writes[0] == 1 && master.writes[1] == 1 && master.writes[2] == 1);
    assert(master.reads[0] == 1 && master.reads[1] == 8 && master.reads[2] >= 3);
    assert(master.fetches == 7 && slave.fetches == 0);
    assert(state(b).r[0] == 0 && kega_sh2_cycles(b) == 0);

    /* Branch budget cannot split its delay slot; skipped opcode must not run. */
    put16(&master, 0x200, 0xa001); put16(&master, 0x202, 0xe007);
    put16(&master, 0x204, 0xe009); put16(&master, 0x206, 0x0009);
    start(a, &master, 0x200);
    assert(kega_sh2_run(a, 1) == 3);
    s = state(a); assert(s.r[0] == 7 && s.pc == 0x206 && !s.delay);
    s.data_array[37] = 0xa5; s.peri_regs[11] = 0x89abcdefu;
    assert(kega_sh2_set_state(a, &s));
    KegaSh2State saved = s;
    assert(kega_sh2_run(a, 1) == 1);
    assert(kega_sh2_set_state(a, &saved)); master.reported_cycles = saved.cycles;
    assert(kega_sh2_run(a, 1) == 1 && state(a).pc == 0x208);
    assert(state(a).data_array[37] == 0xa5 && state(a).peri_regs[11] == 0x89abcdefu);
    s = state(a); s.version = 0; assert(!kega_sh2_set_state(a, &s));

    /* Host wait-state charging and stopping do not charge unused slice time. */
    start(a, &master, 0x400); s = state(a); s.r[1] = 0x500;
    assert(kega_sh2_set_state(a, &s)); put16(&master, 0x400, 0x2100);
    master.wait = 2; assert(kega_sh2_run(a, 1) == 3);
    master.wait = 0; master.stop_on_write = true;
    s = state(a); s.pc = 0x400; assert(kega_sh2_set_state(a, &s));
    assert(kega_sh2_run(a, 100) == 1); master.stop_on_write = false;

    /* External IRQ entry, stack frame and RTE delay slot. */
    start(a, &master, 0x500); master.clear_irq = true;
    put32(&master, 68 * 4, 0x600); put16(&master, 0x600, 0x002b);
    put16(&master, 0x602, 0x7701);
    assert(kega_sh2_set_irq(a, 8) && kega_sh2_run(a, 1) == 13);
    s = state(a); assert(s.pc == 0x600 && s.r[15] == 0x1ef8 && s.sr == 0x80);
    assert(rl(&master, a, s.r[15]) == 0x500 && rl(&master, a, s.r[15] + 4) == 0);
    assert(kega_sh2_run(a, 1) == 5);
    s = state(a); assert(s.pc == 0x500 && s.r[15] == 0x1f00 && s.sr == 0 && s.r[7] == 1);
    unsigned acks = master.acknowledgements;
    put32(&master, 80 * 4, 0x700);
    assert(kega_sh2_set_internal_irq(a, 6, 80) && kega_sh2_run(a, 1) == 13);
    s = state(a); assert(s.pc == 0x700 && !s.pending_int_irq && master.acknowledgements == acks);

    /* IRQ raised by a delay-slot memory callback is taken after the slot. */
    start(a, &master, 0x200); master.raise_at = 0x202;
    assert(kega_sh2_run(a, 1) == 16);
    s = state(a); assert(s.r[0] == 7 && s.pc == 0x600);
    assert(rl(&master, a, s.r[15]) == 0x206);

    /* A memory callback may execute the peer CPU without changing ownership. */
    start(a, &master, 0x300); put16(&master, 0x300, 0xe001);
    put16(&slave, 0x100, 0xe02a); master.peer = b; master.nested = true;
    assert(kega_sh2_run(a, 1) == 1);
    assert(state(a).r[0] == 1 && state(b).r[0] == 42);
    assert(kega_sh2_cycles(a) == 1 && kega_sh2_cycles(b) == 1);
    kega_sh2_destroy(a); kega_sh2_destroy(b);
    puts("SH2 reference: two contexts, memory widths, cycles, delay slots, IRQ/RTE and state passed");
    return 0;
}
