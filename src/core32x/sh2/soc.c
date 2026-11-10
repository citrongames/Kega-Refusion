/*
 * Kega Refusion SH-2 SoC, adapted from PicoDrive sh2soc.c at 07abb00550528f7b.
 * (C) notaz, 2013; (C) irixxxx, 2019-2024.
 * Non-commercial terms: third_party/picodrive32x/COPYING.
 * Changed: per-instance host callbacks, native-cycle deadlines, bounded queued
 * DMA, safe division overflow, FRT and pointer-free peripheral state.
 *
 * FRT/DIVU behavior cross-checked against ares dc4f4659d49693ac91705389e6c37734c016bf16.
 * Copyright (c) 2004-2025 ares team, Near et al.
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "soc.h"
#include <picodrive32x/cpu/sh2/sh2.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { DMA_DE = 1, DMA_TE = 2, DMA_IE = 4, DMA_AR = 0x200,
       DMA_DME = 1, DMA_NMIF = 2, DMA_AE = 4 };
struct KegaSh2Soc {
    KegaSh2 *cpu;
    SH2 *native;
    KegaSh2SocHost host;
    KegaSh2SocState s;
    KegaSh2Soc *peer;
    uint64_t notified;
    bool advancing;
    unsigned irq_level, irq_vector;
};
static uint64_t add_time(uint64_t a, uint64_t b) {
    return b >= UINT64_MAX - a ? UINT64_MAX : a + b;
}
static uint32_t get32(const KegaSh2Soc *c, unsigned a) {
    return c->native->peri_regs[(a & 0x1fc) / 4];
}
static void put32(KegaSh2Soc *c, unsigned a, uint32_t v) {
    c->native->peri_regs[(a & 0x1fc) / 4] = v;
}
static uint8_t get8(const KegaSh2Soc *c, unsigned a) {
    return (uint8_t)(get32(c, a) >> ((3 - (a & 3)) * 8));
}
static void put8(KegaSh2Soc *c, unsigned a, uint8_t v) {
    unsigned shift = (3 - (a & 3)) * 8;
    put32(c, a, (get32(c, a) & ~(255u << shift)) | (uint32_t)v << shift);
}
static uint16_t get16(const KegaSh2Soc *c, unsigned a) {
    return (uint16_t)(get32(c, a) >> ((a & 2) ? 0 : 16));
}
static void put16(KegaSh2Soc *c, unsigned a, uint16_t v) {
    put8(c, a, (uint8_t)(v >> 8)); put8(c, a + 1, (uint8_t)v);
}
static uint64_t current(const KegaSh2Soc *c) {
    uint64_t cpu = kega_sh2_cycles(c->cpu);
    return cpu > c->s.now ? cpu : c->s.now;
}
#define CAUSE(n) (UINT32_C(1) << (n))
#define ALL_CAUSES ((UINT32_C(1) << KEGA_SH2_SOC_IRQ_CAUSES) - 1)
static void cause_parameters(const KegaSh2Soc *c, unsigned cause, unsigned *level, unsigned *vector) {
    switch (cause) {
    case KEGA_SH2_SOC_FRT_CAPTURE: *level=get8(c,0x60)&15; *vector=get8(c,0x66)&127; break;
    case KEGA_SH2_SOC_FRT_COMPARE: *level=get8(c,0x60)&15; *vector=get8(c,0x67)&127; break;
    case KEGA_SH2_SOC_FRT_OVERFLOW: *level=get8(c,0x60)&15; *vector=get8(c,0x68)&127; break;
    case KEGA_SH2_SOC_DMA0: case KEGA_SH2_SOC_DMA1:
        *level=get8(c,0xe2)&15; *vector=get32(c,0x1a0+(cause-KEGA_SH2_SOC_DMA0)*8)&127; break;
    case KEGA_SH2_SOC_WDT: *level=get8(c,0xe3)>>4; *vector=get8(c,0xe4)&127; break;
    case KEGA_SH2_SOC_SCI_ERROR: *level=get8(c,0x60)>>4; *vector=get8(c,0x62)&127; break;
    case KEGA_SH2_SOC_SCI_RX: *level=get8(c,0x60)>>4; *vector=get8(c,0x63)&127; break;
    case KEGA_SH2_SOC_SCI_TX: *level=get8(c,0x60)>>4; *vector=get8(c,0x64)&127; break;
    case KEGA_SH2_SOC_SCI_END: *level=get8(c,0x60)>>4; *vector=get8(c,0x65)&127; break;
    default: *level=get8(c,0xe2)>>4; *vector=get32(c,0x10c)&127; break;
    }
}
static uint32_t active_causes(const KegaSh2Soc *c) {
    uint32_t active=0;
    unsigned frt=get8(c,0x10)&get8(c,0x11), sci=get8(c,2), ssr=get8(c,4);
    if(frt&0x80)active|=CAUSE(KEGA_SH2_SOC_FRT_CAPTURE);
    if(frt&0x0c)active|=CAUSE(KEGA_SH2_SOC_FRT_COMPARE);
    if(frt&2)active|=CAUSE(KEGA_SH2_SOC_FRT_OVERFLOW);
    if((get32(c,0x18c)&6)==6)active|=CAUSE(KEGA_SH2_SOC_DMA0);
    if((get32(c,0x19c)&6)==6)active|=CAUSE(KEGA_SH2_SOC_DMA1);
    if((get8(c,0x80)&0xe0)==0xa0)active|=CAUSE(KEGA_SH2_SOC_WDT);
    if((sci&0x40)&&(ssr&0x38))active|=CAUSE(KEGA_SH2_SOC_SCI_ERROR);
    if((sci&0x40)&&(ssr&0x40))active|=CAUSE(KEGA_SH2_SOC_SCI_RX);
    if((sci&0x80)&&(ssr&0x80))active|=CAUSE(KEGA_SH2_SOC_SCI_TX);
    if((sci&4)&&(ssr&4))active|=CAUSE(KEGA_SH2_SOC_SCI_END);
    if((get32(c,0x108)&3)==3)active|=CAUSE(KEGA_SH2_SOC_DIVU);
    return active;
}
static void resample_irq(KegaSh2Soc *c) {
    uint32_t active=active_causes(c), selected=0;
    unsigned best_level=0, best_vector=0;
    c->s.irq_pending = active;
    static const unsigned order[KEGA_SH2_SOC_IRQ_CAUSES] = {
        KEGA_SH2_SOC_DIVU, KEGA_SH2_SOC_DMA0, KEGA_SH2_SOC_DMA1, KEGA_SH2_SOC_WDT,
        KEGA_SH2_SOC_SCI_ERROR, KEGA_SH2_SOC_SCI_RX, KEGA_SH2_SOC_SCI_TX, KEGA_SH2_SOC_SCI_END,
        KEGA_SH2_SOC_FRT_CAPTURE, KEGA_SH2_SOC_FRT_COMPARE, KEGA_SH2_SOC_FRT_OVERFLOW
    };
    for(unsigned index=0;index<KEGA_SH2_SOC_IRQ_CAUSES;++index) {
        unsigned cause=order[index],level,vector;
        if(!(c->s.irq_pending&CAUSE(cause)))continue;
        cause_parameters(c,cause,&level,&vector);
        if(level>best_level) {
            best_level=level;best_vector=vector;selected=CAUSE(cause);
        }
    }
    c->s.irq_selected=selected;
    if(best_level!=c->irq_level || best_vector!=c->irq_vector) {
        c->irq_level=best_level;c->irq_vector=best_vector;
        if(c->host.interrupt)c->host.interrupt(c->host.user,c->cpu,best_level,best_vector);
        else kega_sh2_set_internal_irq(c->cpu,best_level,best_vector);
    }
}
static void raise_cause(KegaSh2Soc *c, unsigned cause) {
    c->s.irq_pending|=CAUSE(cause);
}
static void internal_ack(void *user, KegaSh2 *cpu, unsigned level, unsigned vector) {
    KegaSh2Soc *c=user;
    (void)cpu; (void)level; (void)vector;
    /* Level-sensitive flags survive acknowledge. Republish even for the same
     * source; the new SR mask blocks it until the ISR clears it or returns. */
    c->irq_level=UINT_MAX;resample_irq(c);
}
static unsigned frt_divisor(const KegaSh2Soc *c) {
    static const unsigned divisors[4] = {8, 32, 128, 0};
    return divisors[get8(c, 0x16) & 3]; /* 3 selects an external clock. */
}
static unsigned wdt_divisor(const KegaSh2Soc *c) {
    static const unsigned divisors[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};
    return divisors[get8(c, 0x80) & 7];
}
static uint64_t ticks(uint64_t delta, uint32_t *phase, unsigned divisor) {
    uint64_t result = delta / divisor;
    unsigned tail = *phase + (unsigned)(delta % divisor);
    if (tail >= divisor) { ++result; tail -= divisor; }
    *phase = tail; return result;
}
static unsigned distance16(unsigned from, unsigned to) {
    unsigned distance = (to - from) & 65535;
    return distance ? distance : 65536;
}
static void frt_irq(KegaSh2Soc *c, uint8_t changed) {
    if (changed & 0x80) raise_cause(c, KEGA_SH2_SOC_FRT_CAPTURE);
    if (changed & 0x0c) raise_cause(c, KEGA_SH2_SOC_FRT_COMPARE);
    if (changed & 2) raise_cause(c, KEGA_SH2_SOC_FRT_OVERFLOW);
}
static void sync_timers(KegaSh2Soc *c, uint64_t now) {
    uint64_t count;
    unsigned divisor;
    if (now < c->s.now) return;
    if (now == c->s.now && c->s.frt_last == now && c->s.wdt_last == now) return;
    divisor = frt_divisor(c);
    c->s.now = now;
    if (divisor) {
        unsigned counter = get16(c, 0x12), a = distance16(counter, c->s.ocra);
        unsigned b = distance16(counter, c->s.ocrb), wrap = 65536 - counter;
        uint8_t old = get8(c, 0x11), flags = 0;
        count = ticks(now - c->s.frt_last, &c->s.frt_phase, divisor);
        if (count >= a) flags |= 8;
        if (count >= b) flags |= 4;
        if (count >= wrap) flags |= 2;
        if ((old & 1) && count >= a) {
            uint64_t remainder = count - a;
            unsigned period = c->s.ocra ? c->s.ocra : 65536;
            /* After compare A the counter restarts at zero each period. */
            flags = 8;
            if (b <= a || (c->s.ocrb && c->s.ocrb <= period && remainder >= c->s.ocrb)) flags |= 4;
            if (wrap <= a || (period == 65536 && remainder >= 65536)) flags |= 2;
            counter = (unsigned)(remainder % period);
        } else counter = (counter + (unsigned)(count & 65535)) & 65535;
        put16(c, 0x12, (uint16_t)counter);
        put8(c, 0x11, old | flags);
        c->s.frt_last = now;
        frt_irq(c, flags);
    }
    c->s.frt_last = now;
    if (get8(c, 0x80) & 0x20) {
        unsigned old_counter = get8(c, 0x81);
        count = ticks(now - c->s.wdt_last, &c->s.wdt_phase, wdt_divisor(c));
        put8(c, 0x81, (uint8_t)(old_counter + count));
        c->s.wdt_last = now;
        if (count >= 256 - old_counter) {
            uint8_t control = get8(c, 0x80);
            put8(c, 0x80, control | 0x80);
            /* Interval mode generates IRQ; watchdog reset output is not wired. */
            if (!(control & 0x40)) raise_cause(c, KEGA_SH2_SOC_WDT);
            else put8(c, 0x83, get8(c, 0x83) | 0x80);
        }
    }
    c->s.wdt_last = c->s.now = now;
}
static bool dma_enabled(const KegaSh2Soc *c, unsigned ch) {
    unsigned control = get32(c, 0x18c + ch * 16), dmaor = get32(c, 0x1b0);
    return (dmaor & (DMA_DME | DMA_AE | DMA_NMIF)) == DMA_DME &&
        (control & (DMA_DE | DMA_TE)) == DMA_DE;
}
static bool fifo_dma(const KegaSh2Soc *c, unsigned ch) {
    return ch == 0 && !(get32(c, 0x18c) & DMA_AR) &&
        (get32(c, 0x180) & ~0x20000000u) == 0x4012;
}
static unsigned dma_units(const KegaSh2Soc *c, unsigned ch) {
    unsigned remaining = c->s.dma_remaining[ch];
    if (fifo_dma(c, ch)) return remaining < 4 ? remaining : 4;
    return ((get32(c, 0x18c + 16 * ch) >> 10) & 3) == 3 ? 4 : 1;
}
static bool dma_request(const KegaSh2Soc *c, unsigned ch) {
    unsigned control = get32(c, 0x18c + ch * 16);
    if (control & DMA_AR) return true;
    if (get8(c, 0x71 + ch) & 3) return false; /* SCI DMA not connected. */
    if (fifo_dma(c, ch))
        return c->host.dreq_available && c->host.dreq_pop &&
            c->host.dreq_available(c->host.user, c->cpu) >= 4;
    return ch == 1 && c->s.dreq_tokens[1] &&
        (get32(c, 0x194) & 0xdffffff0u) == 0x4030;
}
static void dma_refresh(KegaSh2Soc *c, bool reset_count, unsigned changed_channel) {
    for (unsigned ch = 0; ch < 2; ++ch) {
        if (!dma_enabled(c, ch)) {
            c->s.dma_active[ch] = 0; c->s.dma_due[ch] = UINT64_MAX; continue;
        }
        if (!c->s.dma_active[ch] || (reset_count && ch == changed_channel)) {
            unsigned count = get32(c, 0x188 + 16 * ch) & 0xffffff;
            c->s.dma_remaining[ch] = count ? count : 0x1000000;
            c->s.dma_active[ch] = 1;
        }
        if (!dma_request(c, ch)) c->s.dma_due[ch] = UINT64_MAX;
        else if (c->s.dma_due[ch] == UINT64_MAX)
            c->s.dma_due[ch] = add_time(c->s.now, dma_units(c, ch) * 2);
    }
}
static uint64_t calculate_deadline(const KegaSh2Soc *c, uint64_t now) {
    uint64_t deadline = UINT64_MAX, candidate;
    unsigned divisor;
    if (!c) return deadline;
    for (unsigned ch = 0; ch < 2; ++ch)
        if (c->s.dma_due[ch] < deadline) deadline = c->s.dma_due[ch];
    if (get8(c, 0x80) & 0x20) {
        candidate = add_time(c->s.wdt_last,
            (uint64_t)(256 - get8(c, 0x81)) * wdt_divisor(c) - c->s.wdt_phase);
        if (candidate < deadline) deadline = candidate;
    }
    divisor = frt_divisor(c);
    if (divisor) {
        unsigned count=get16(c,0x12), distance=UINT_MAX;
        uint8_t status=get8(c,0x11);
        /* CCLRA changes the counter, not an externally observable event.
         * Bulk timer synchronization preserves all flag/counter transitions.
         * A sticky flag already asserts its level-sensitive IRQ; repeated
         * matches cannot change that request until software clears the flag. */
        unsigned pending=get8(c,0x10) & ~status & 0x0e;
        for(unsigned flag=2;flag<=8;flag<<=1) {
            if(!(pending&flag))continue;
            unsigned target=flag==8 ? c->s.ocra : flag==4 ? c->s.ocrb : 0;
            unsigned ticks_to=distance16(count,target);
            if(status&1) {
                unsigned first_a=distance16(count,c->s.ocra);
                if(ticks_to>first_a) {
                    unsigned from_zero=target ? target : 65536;
                    unsigned period=c->s.ocra ? c->s.ocra : 65536;
                    if(from_zero>period)continue; /* A clears before this match. */
                    ticks_to=first_a+from_zero;
                }
            }
            if(ticks_to<distance)distance=ticks_to;
        }
        if(distance!=UINT_MAX) {
            candidate=add_time(c->s.frt_last,(uint64_t)distance*divisor-c->s.frt_phase);
            if(candidate<deadline)deadline=candidate;
        }
    }
    return deadline < now ? now : deadline;
}
static void notify(KegaSh2Soc *c) {
    resample_irq(c);
    uint64_t deadline = calculate_deadline(c, c->s.now);
    if (deadline != c->notified) {
        c->notified = deadline;
        if (c->host.schedule_changed) c->host.schedule_changed(c->host.user, c->cpu, deadline);
    }
}
uint64_t kega_sh2_soc_next_deadline(const KegaSh2Soc *c, uint64_t now) {
    if (!c) return UINT64_MAX;
    /* Register changes, DREQs and state restores publish the new deadline.
       A reentrant query during DMA must see the in-progress register values. */
    if (c->advancing || now < c->s.now) return calculate_deadline(c, now);
    return c->notified < now ? now : c->notified;
}
static void touch(KegaSh2Soc *c) { sync_timers(c, current(c)); }

static void sci_trigger(KegaSh2Soc *c) {
    KegaSh2Soc *peer = c->peer;
    if (!peer || !(get8(c, 2) & 0x20) || (get8(c, 4) & 0x80) || !(get8(peer, 2) & 0x10)) return;
    if (get8(peer, 4) & 0x40) {
        put8(peer, 4, get8(peer, 4) | 0x20); /* Receiver overrun. */
        raise_cause(peer, KEGA_SH2_SOC_SCI_ERROR);
    } else {
        put8(peer, 5, get8(c, 3)); put8(peer, 4, get8(peer, 4) | 0x40);
        raise_cause(peer, KEGA_SH2_SOC_SCI_RX);
    }
    put8(c, 4, get8(c, 4) | 0x84);
    raise_cause(c, KEGA_SH2_SOC_SCI_TX);
    raise_cause(c, KEGA_SH2_SOC_SCI_END);
    resample_irq(peer);
}
static void divide(KegaSh2Soc *c, bool wide) {
    int64_t divisor = (int32_t)get32(c, 0x100), dividend, quotient = 0, remainder;
    uint32_t result;
    bool overflow = false;
    if (wide) {
        uint64_t bits = (uint64_t)get32(c, 0x110) << 32 | get32(c, 0x114);
        dividend = bits & (UINT64_C(1) << 63) ? -1 - (int64_t)~bits : (int64_t)bits;
    } else dividend = (int32_t)get32(c, 0x104);
    remainder = (int32_t)get32(c, 0x110);
    if (!divisor) { result = INT32_MAX; overflow = true; }
    else if (dividend == INT64_MIN && divisor == -1) {
        result = INT32_MAX; remainder = 0; overflow = true;
    } else {
        quotient = dividend / divisor; remainder = dividend % divisor;
        overflow = quotient > INT32_MAX || quotient < INT32_MIN;
        result = quotient > INT32_MAX ? (uint32_t)INT32_MAX :
                 quotient < INT32_MIN ? UINT32_C(0x80000000) : (uint32_t)quotient;
    }
    put32(c, 0x104, result); put32(c, 0x114, result); put32(c, 0x11c, result);
    put32(c, 0x110, (uint32_t)remainder); put32(c, 0x118, (uint32_t)remainder);
    if (overflow) {
        put32(c, 0x108, get32(c, 0x108) | 1);
        raise_cause(c, KEGA_SH2_SOC_DIVU);
    }
}
static unsigned canonical(unsigned a) {
    a &= 0x1ff;
    return a >= 0x118 && a < 0x120 ? a - 8 : a;
}
static uint8_t read_byte(const KegaSh2Soc *c, unsigned a) {
    a = canonical(a);
    if (a == 0x14 || a == 0x15) {
        unsigned v = get8(c, 0x17) & 0x10 ? c->s.ocrb : c->s.ocra;
        return (uint8_t)(v >> ((a & 1) ? 0 : 8));
    }
    return get8(c, a);
}
static void dma_write(KegaSh2Soc *c, unsigned a, uint32_t v) {
    uint32_t old = get32(c, a);
    if (a == 0x188 || a == 0x198) v &= 0xffffff;
    if (a == 0x18c || a == 0x19c) v = (v & ~DMA_TE) | (old & v & DMA_TE);
    if (a == 0x1b0) v = (v & 9) | (old & v & 6);
    put32(c, a, v);
    dma_refresh(c, a == 0x188 || a == 0x198, a >= 0x190 ? 1 : 0);
}
static void write_byte(KegaSh2Soc *c, unsigned a, uint8_t v) {
    uint8_t old;
    a = canonical(a); old = get8(c, a);
    if (a >= 0x180 && a <= 0x1b3) {
        unsigned shift = (3 - (a & 3)) * 8;
        dma_write(c, a & ~3u, (get32(c, a) & ~(255u << shift)) | (uint32_t)v << shift);
        return;
    }
    if (a == 0x80 || a == 0x82) {
        if (a == 0x80) c->s.wdt_key = v; else c->s.rst_key = v;
        return;
    }
    if (a == 0x81) {
        if (c->s.wdt_key == 0xa5) {
            uint8_t control = get8(c, 0x80);
            put8(c, 0x80, (v & 0x67) | (v & control & 0x80));
            if (!(v & 0x20)) put8(c, 0x81, 0);
        } else if (c->s.wdt_key == 0x5a) put8(c, 0x81, v);
        if (c->s.wdt_key == 0xa5 || c->s.wdt_key == 0x5a) {
            c->s.wdt_phase = 0; c->s.wdt_last = c->s.now;
        }
        c->s.wdt_key = 0;
        return;
    }
    if (a == 0x83) {
        if (c->s.rst_key == 0xa5) put8(c, 0x83, get8(c, 0x83) & (v | 0x7f));
        else if (c->s.rst_key == 0x5a) put8(c, 0x83, (get8(c, 0x83) & 0x80) | (v & 0x60));
        c->s.rst_key = 0; return;
    }
    if (a == 0x14 || a == 0x15) {
        uint16_t *ocr = get8(c, 0x17) & 0x10 ? &c->s.ocrb : &c->s.ocra;
        *ocr = (uint16_t)((a & 1) ? (*ocr & 0xff00) | v : (*ocr & 255) | (uint16_t)v << 8);
        return;
    }
    if (a == 4) v = (old & (v | 6)) | (v & 1);
    if (a == 0x10) v = (v & 0x8e) | 1;
    if (a == 0x11) v = (old & v & 0x8e) | (v & 1);
    if (a == 0x16) { v &= 0x83; c->s.frt_phase = 0; }
    if (a == 0x17) v = (v & 0x13) | 0xe0;
    if (a == 0x18 || a == 0x19) return; /* Input capture is read-only. */
    put8(c, a, v);
    if (a == 2 || a == 4) {
        sci_trigger(c);
        if (a == 2 && c->peer) sci_trigger(c->peer);
    }
    if (a == 0x10) frt_irq(c, get8(c, 0x11));
    if (a == 0x107) divide(c, false);
    if (a == 0x117) divide(c, true);
    if (a >= 0x108 && a < 0x10c) put32(c, 0x108, get32(c, 0x108) & 3);
    if (a >= 0x10c && a < 0x110) put32(c, 0x10c, get32(c, 0x10c) & 127);
    if ((a & 0x1c0) == 0x140) {
        c->native->state &= ~SH2_STATE_CPOLL; c->native->poll_cnt = 0;
    }
}
uint8_t kega_sh2_soc_read8(KegaSh2Soc *c, uint32_t a) {
    uint8_t v; if (!c) return 0; touch(c); v = read_byte(c, a); notify(c); return v;
}
uint16_t kega_sh2_soc_read16(KegaSh2Soc *c, uint32_t a) {
    uint16_t v; if (!c) return 0; touch(c); a &= 0x1fe;
    v = (uint16_t)((uint16_t)read_byte(c, a) << 8 | read_byte(c, a + 1)); notify(c); return v;
}
uint32_t kega_sh2_soc_read32(KegaSh2Soc *c, uint32_t a) {
    uint32_t v = 0; if (!c) return 0; touch(c); a &= 0x1fc;
    for (unsigned i = 0; i < 4; ++i) v = v << 8 | read_byte(c, a + i);
    if (a == 0x18c || a == 0x19c) c->native->poll_cnt = 0;
    notify(c); return v;
}
void kega_sh2_soc_write8(KegaSh2Soc *c, uint32_t a, uint8_t v) {
    if (!c) return;
    touch(c); write_byte(c, a, v); notify(c);
}
void kega_sh2_soc_write16(KegaSh2Soc *c, uint32_t a, uint16_t v) {
    if (!c) return;
    touch(c); a &= 0x1fe;
    write_byte(c, a, (uint8_t)(v >> 8)); write_byte(c, a + 1, (uint8_t)v); notify(c);
}
void kega_sh2_soc_write32(KegaSh2Soc *c, uint32_t a, uint32_t v) {
    if (!c) return;
    touch(c); a &= 0x1fc;
    if (a >= 0x180 && a <= 0x1b0) dma_write(c, a, v);
    else for (unsigned i = 0; i < 4; ++i) write_byte(c, a + i, (uint8_t)(v >> (24 - i * 8)));
    notify(c);
}

static void dma_complete(KegaSh2Soc *c, unsigned ch) {
    unsigned a = 0x18c + ch * 16;
    put32(c, a, get32(c, a) | DMA_TE);
    c->s.dma_active[ch] = 0; c->s.dma_due[ch] = UINT64_MAX;
    c->native->state &= ~(SH2_STATE_SLEEP | SH2_STATE_CPOLL);
    raise_cause(c, KEGA_SH2_SOC_DMA0 + ch);
}
static bool dma_transfer(KegaSh2Soc *c, unsigned ch, unsigned *used) {
    unsigned base = 0x180 + ch * 16, ctl = get32(c, base + 12);
    uint32_t source = get32(c, base), destination = get32(c, base + 4);
    uint32_t initial_destination = destination;
    unsigned size = (ctl >> 10) & 3, units = dma_units(c, ch), bytes = 1u << (size == 3 ? 2 : size);
    bool fifo = fifo_dma(c, ch);
    if ((source & (bytes - 1)) || (destination & (bytes - 1)) ||
        (fifo && size != 1) || units > c->s.dma_remaining[ch]) {
        put32(c, 0x1b0, get32(c, 0x1b0) | DMA_AE);
        dma_refresh(c, false, 0); return false;
    }
    for (unsigned i = 0; i < units; ++i) {
        uint32_t value;
        if (fifo) value = c->host.dreq_pop(c->host.user, c->cpu);
        else if (size == 0) value = c->host.read8(c->host.user, c->cpu, source);
        else if (size == 1) value = c->host.read16(c->host.user, c->cpu, source);
        else value = c->host.read32(c->host.user, c->cpu, source);
        if (size == 0) c->host.write8(c->host.user, c->cpu, destination, (uint8_t)value);
        else if (size == 1) c->host.write16(c->host.user, c->cpu, destination, (uint16_t)value);
        else c->host.write32(c->host.user, c->cpu, destination, value);
        if (!fifo) {
            if (size == 3 || ((ctl >> 12) & 3) == 1) source += bytes;
            else if (((ctl >> 12) & 3) == 2) source -= bytes;
        }
        if (size == 3 || ((ctl >> 14) & 3) == 1) destination += bytes;
        else if (((ctl >> 14) & 3) == 2) destination -= bytes;
    }
    if (size == 3) destination = ((ctl >> 14) & 3) == 1 ? initial_destination + 16 :
        ((ctl >> 14) & 3) == 2 ? initial_destination - 16 : initial_destination;
    put32(c, base, source); put32(c, base + 4, destination);
    c->s.dma_remaining[ch] -= units; put32(c, base + 8, c->s.dma_remaining[ch] & 0xffffff);
    if (!(ctl & DMA_AR) && !fifo && c->s.dreq_tokens[ch]) --c->s.dreq_tokens[ch];
    *used = units;
    if (!c->s.dma_remaining[ch]) dma_complete(c, ch);
    return true;
}
void kega_sh2_soc_advance(KegaSh2Soc *c, uint64_t target) {
    unsigned work = 0;
    if (!c || c->advancing || target < c->s.now) return;
    /* Idle DMA has no bus work. An enabled serial port can change from the
       peer CPU, so keep the full IRQ resample there. Before a timer deadline
       counting and masked flags still advance without changing the IRQ line. */
    if (!(get32(c, 0x1b0) & DMA_DME) && !(get8(c, 2) & 0x30) && target < c->notified &&
        !c->s.dma_active[0] && !c->s.dma_active[1] &&
        c->s.dma_due[0] == UINT64_MAX && c->s.dma_due[1] == UINT64_MAX) {
        uint32_t pending = c->s.irq_pending;
        sync_timers(c, target);
        /* Timer matches can latch masked flags. The published interrupt
           selection is unchanged until an enabled event or register write. */
        c->s.irq_pending = pending;
        return;
    }
    c->advancing = true; sync_timers(c, target); dma_refresh(c, false, 0);
    while (work < 4) {
        unsigned selected = 2, first = get32(c, 0x1b0) & 8 ? c->s.dma_turn : 0;
        for (unsigned index = 0; index < 2; ++index) {
            unsigned ch = (first + index) & 1;
            if (dma_enabled(c, ch) && c->s.dma_active[ch] && dma_request(c, ch) &&
                c->s.dma_due[ch] <= target && dma_units(c, ch) <= 4 - work &&
                (selected == 2 || c->s.dma_due[ch] < c->s.dma_due[selected])) selected = ch;
        }
        if (selected == 2) break;
        unsigned used = 0;
        uint64_t completed = c->s.dma_due[selected];
        if (!dma_transfer(c, selected, &used)) break;
        work += used; c->s.dma_turn = (uint8_t)(selected ^ 1);
        if (c->s.dma_active[selected]) c->s.dma_due[selected] = dma_request(c, selected)
            ? add_time(completed, dma_units(c, selected) * 2) : UINT64_MAX;
    }
    /* A large caller jump never triggers an unbounded catch-up loop. */
    if (work == 4) for (unsigned ch = 0; ch < 2; ++ch)
        if (c->s.dma_due[ch] <= target) c->s.dma_due[ch] = add_time(target, dma_units(c, ch) * 2);
    c->advancing = false; notify(c);
}
void kega_sh2_soc_dreq_at(KegaSh2Soc *c, unsigned channel, uint64_t now) {
    if (!c || channel > 1) return;
    sync_timers(c, now > c->s.now ? now : c->s.now);
    if (channel == 1 && c->s.dreq_tokens[1] < 255) ++c->s.dreq_tokens[1];
    dma_refresh(c, false, 0); notify(c);
}
void kega_sh2_soc_dreq(KegaSh2Soc *c, unsigned channel) {
    if (c) kega_sh2_soc_dreq_at(c, channel, current(c));
}
void kega_sh2_soc_capture(KegaSh2Soc *c) {
    if (!c) return;
    touch(c); put16(c, 0x18, get16(c, 0x12));
    put8(c, 0x11, get8(c, 0x11) | 0x80);
    frt_irq(c, 0x80);
    notify(c);
}
uint64_t kega_sh2_soc_now(const KegaSh2Soc *c) { return c ? c->s.now : 0; }
KegaSh2Soc *kega_sh2_soc_create(KegaSh2 *cpu, const KegaSh2SocHost *host) {
    KegaSh2Soc *c;
    if (!cpu || !host || !host->read8 || !host->read16 || !host->read32 ||
        !host->write8 || !host->write16 || !host->write32) return NULL;
    c = calloc(1, sizeof(*c)); if (!c) return NULL;
    c->cpu = cpu; c->native = kega_sh2_native(cpu); c->host = *host;
    kega_sh2_set_internal_irq_ack(cpu, internal_ack, c);
    kega_sh2_soc_reset(c, kega_sh2_cycles(cpu)); return c;
}
void kega_sh2_soc_destroy(KegaSh2Soc *c) {
    if (c) kega_sh2_set_internal_irq_ack(c->cpu, NULL, NULL);
    free(c);
}
void kega_sh2_soc_set_peer(KegaSh2Soc *c, KegaSh2Soc *peer) { if (c) c->peer = peer; }
void kega_sh2_soc_reset(KegaSh2Soc *c, uint64_t now) {
    if (!c) return;
    memset(&c->s, 0, sizeof(c->s)); memset(c->native->peri_regs, 0, sizeof(c->native->peri_regs));
    c->s.version = KEGA_SH2_SOC_STATE_VERSION;
    c->s.now = c->s.frt_last = c->s.wdt_last = now;
    c->s.ocra = c->s.ocrb = 0xffff;
    c->s.dma_due[0] = c->s.dma_due[1] = UINT64_MAX;
    put8(c, 1, 0xff); put8(c, 3, 0xff); put8(c, 4, 0x84);
    put8(c, 0x10, 1); put8(c, 0x17, 0xe0);
    c->notified = 0; c->irq_level = UINT_MAX; notify(c);
}
bool kega_sh2_soc_get_state(const KegaSh2Soc *c, KegaSh2SocState *state) {
    if (!c || !state || c->advancing) return false;
    *state = c->s; memcpy(state->regs, c->native->peri_regs, sizeof(state->regs)); return true;
}
bool kega_sh2_soc_validate_state(const KegaSh2SocState *state) {
    if (!state || state->version != KEGA_SH2_SOC_STATE_VERSION ||
        state->frt_last > state->now || state->wdt_last > state->now ||
        state->frt_phase >= 128 || state->wdt_phase >= 8192 || state->dma_turn > 1 ||
        ((state->irq_pending | state->irq_selected) & ~ALL_CAUSES) ||
        (state->irq_selected && (state->irq_selected & (state->irq_selected - 1)))) return false;
    static const unsigned frt_frequency[4] = {8,32,128,1};
    static const unsigned wdt_frequency[8] = {2,64,128,256,512,1024,4096,8192};
    unsigned frt_mode = (state->regs[0x14 / 4] >> 8) & 3;
    unsigned wdt_mode = (state->regs[0x80 / 4] >> 24) & 7;
    if (state->frt_phase >= frt_frequency[frt_mode] || state->wdt_phase >= wdt_frequency[wdt_mode]) return false;
    for (unsigned ch = 0; ch < 2; ++ch)
        if (state->dma_active[ch] > 1 || state->dma_remaining[ch] > 0x1000000 ||
            (state->dma_active[ch] && !state->dma_remaining[ch])) return false;
    return true;
}
bool kega_sh2_soc_set_state(KegaSh2Soc *c, const KegaSh2SocState *state) {
    if(!c || c->advancing || !kega_sh2_soc_validate_state(state)) return false;
    c->s = *state; memcpy(c->native->peri_regs, state->regs, sizeof(state->regs));
    c->notified = 0; c->irq_level = UINT_MAX; notify(c); return true;
}
