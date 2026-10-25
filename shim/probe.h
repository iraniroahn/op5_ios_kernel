#pragma once
#include <stdint.h>

/*
 * cheeseburger (OnePlus 5, MSM8998) hardware probe, run by the shim when
 * there is no xnu payload. Every page is read-only towards the hardware
 * except PSCI CPU_ON/CPU_OFF and the SPMI observer command register.
 */

/* One record per core, written by that core with the MMU off (uncached). */
struct cpu_rec {
	uint64_t midr, mpidr, mmfr0, mmfr1, mmfr2, isar0, isar1, pfr0;
	uint64_t ticks;		/* speed_ticks(): smaller = faster clock */
	uint64_t done;
};

#define PROBE_CPUS	8
extern volatile struct cpu_rec cpu_recs[PROBE_CPUS];
extern volatile int64_t cpu_on_ret[PROBE_CPUS];

void probe_cpu_table(void);
void probe_mem(uintptr_t dtb);
void probe_pmic(void);
void probe_keys_live(unsigned seconds);
void probe_hold(unsigned seconds);
void probe_osm(void);
int probe_osm_gold_enabled(void);
uint32_t probe_osm_gold_desired(void);
void probe_osm_gold_request(uint32_t idx);
uint32_t probe_osm_gold_mhz(void);
void probe_touch(unsigned live_s);
int probe_spmi_reg(uint16_t ppid, uint8_t addr);
int probe_spmi_owner(uint16_t ppid, uint32_t *owner);
int probe_spmi_write(uint16_t ppid, uint8_t addr, const uint8_t *buf, unsigned n);
void probe_gfx(void);
void probe_scale(void);
