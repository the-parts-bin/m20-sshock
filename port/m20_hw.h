// System Shock on the m20: the span engine's registers and the cache
// instructions the engine's code uses (port/m20_engine.c, m9b_shock.c). On
// the board they're the register accesses and instructions themselves; in
// the host test harness (tools/host/host.c, not RISC-V) the registers go to
// the engine's C model (../../core/firmware/span_emu.c), SDRAM_OFF is
// checked against the SDRAM window, and the cache instructions do nothing.
#ifndef M20_HW_H
#define M20_HW_H

#include <stdint.h>

#ifdef __riscv
#define SPAN_W(r, v)     (((volatile uint32_t *)0x490000u)[r] = (v))
#define SPAN_R(r)        (((volatile uint32_t *)0x490000u)[r])
#define SDRAM_OFF(p)     ((uint32_t)(uintptr_t)(p) - 0x800000u)
#define CBO_CLEAN(a)     asm volatile("cbo.clean (%0)" :: "r"(a) : "memory")
#define CBO_FLUSH(a)     asm volatile("cbo.flush (%0)" :: "r"(a) : "memory")
#define PREFETCH_R64(a)  asm volatile("prefetch.r 64(%0)" :: "r"(a) : "memory")   // the line after a's
#else
void span_emu_write(uint32_t reg, uint32_t v);
uint32_t span_emu_read(uint32_t reg);
uint32_t m20_host_sdram_off(const void *p);   // tools/host/host.c: SDRAM_OFF, checked
#define SPAN_W(r, v)     span_emu_write((r), (v))
#define SPAN_R(r)        span_emu_read(r)
#define SDRAM_OFF(p)     m20_host_sdram_off(p)
#define CBO_CLEAN(a)     ((void)(a))
#define CBO_FLUSH(a)     ((void)(a))
#define PREFETCH_R64(a)  ((void)(a))
#endif

#endif
