/*
 * cheeseburger (OnePlus 5, MSM8998) probe pages. The answers decide the
 * port before any kernel driver is written:
 *   1. per-core feature registers: which cores have the 16K granule xnu
 *      needs, and the ARMv8.0 gaps (LSE, PAN, RCpc) topaz patch 0081 covers
 *   2. the RAM ranges and carve-outs ABL hands over (mkadt.py --highmem)
 *   3. PMIC over the SPMI v3 arbiter (keys, touch rails), the touch I2C
 *      clocks and pins ABL leaves behind
 * Reads only, apart from the SPMI observer command register (EXT_READL).
 * All addresses: mainline msm8998.dtsi / msm8998-oneplus-common.dtsi and
 * the OnePlus msm8998 kernel (synaptics s3320 node).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "probe.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

volatile struct cpu_rec cpu_recs[PROBE_CPUS];
volatile int64_t cpu_on_ret[PROBE_CPUS];

static uint64_t now(void)
{
	__asm__ volatile("isb");
	return rsr("cntpct_el0");
}

static void delay_us(unsigned us)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000000 * us;
	while (now() < end)
		;
}

/* ---- tiny string building (no libc; byte stores are fine on Device memory) ---- */

static char *st(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	*p = 0;
	return p;
}

static char *hx(char *p, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = digits - 1; i >= 0; i--)
		*p++ = hex[(v >> (4 * i)) & 0xf];
	*p = 0;
	return p;
}

static char *dec(char *p, uint64_t v)
{
	char tmp[20];
	int n = 0;
	do {
		tmp[n++] = '0' + v % 10;
		v /= 10;
	} while (v && n < 20);
	while (n)
		*p++ = tmp[--n];
	*p = 0;
	return p;
}

static char *kv(char *p, const char *k, uint64_t v, int digits)
{
	p = st(p, k);
	p = hx(p, v, digits);
	return st(p, " ");
}

void probe_hold(unsigned seconds)
{
	char b[48], *p = b;
	p = st(p, "next page in ");
	p = dec(p, seconds);
	st(p, " s (take a photo)");
	con_skip();
	con_str("WAIT", b);
	delay_us(seconds * 1000000u);
}

/* ---- page 1: CPU feature registers ---- */

#define FIELD(v, lo) (((v) >> (lo)) & 0xf)

void probe_cpu_table(void)
{
	int silver16 = -1, gold16 = -1, lse = 0, pan = 0, rcpc = 0;

	con_str("", "core mpidr midr     mmfr0    mmfr1    isar0");
	for (int i = 0; i < PROBE_CPUS; i++) {
		volatile struct cpu_rec *r = &cpu_recs[i];
		char label[] = "CPU0", b[72], *p = b;
		label[3] = '0' + i;
		if (!r->done) {
			p = st(p, "no report, CPU_ON ret=");
			hx(p, (uint64_t)cpu_on_ret[i], 8);
			con_str(label, b);
			continue;
		}
		uint64_t m0 = r->mmfr0, m1 = r->mmfr1, i0 = r->isar0, i1 = r->isar1;
		int tg16 = FIELD(m0, 20) != 0;
		p = hx(p, r->mpidr & 0xffff, 3);
		p = st(p, " ");
		p = kv(p, "", r->midr, 8);
		p = kv(p, "", m0, 8);
		p = kv(p, "", m1, 8);
		hx(p, i0, 8);
		con_str(label, b);

		p = b;
		p = kv(p, "tg16=", tg16, 1);
		p = kv(p, "tg4=", FIELD(m0, 28) == 0, 1);
		p = kv(p, "lse=", FIELD(i0, 20) >= 2, 1);
		p = kv(p, "pan=", FIELD(m1, 20) != 0, 1);
		p = kv(p, "rcpc=", FIELD(i1, 20) != 0, 1);
		p = kv(p, "aes=", FIELD(i0, 4), 1);
		p = kv(p, "gic=", FIELD(r->pfr0, 24), 1);
		p = st(p, "~");
		p = dec(p, r->ticks ? 384000000ull / r->ticks : 0);	/* 20M adds at 19.2 MHz */
		st(p, "MHz");
		con_str("", b);

		int gold = (r->mpidr & 0xff00) == 0x100;
		if (gold)
			gold16 = gold16 == 0 ? 0 : tg16;
		else
			silver16 = silver16 == 0 ? 0 : tg16;
		lse |= FIELD(i0, 20) >= 2;
		pan |= FIELD(m1, 20) != 0;
		rcpc |= FIELD(i1, 20) != 0;
	}
	con_skip();
	con_str("VERDICT", gold16 == 1 ? "gold cores have 16K pages: xnu runs on gold (as topaz)"
			   : gold16 == 0 ? "NO 16K on gold: needs the 4K-page xnu (topaz 0140)"
			   : "gold cores did not report");
	con_str("", silver16 == 1 ? "silver has 16K too: all 8 cores usable"
		    : silver16 == 0 ? "silver has no 16K: parked, as on topaz"
		    : "silver cores did not report");
	con_str("", lse || pan || rcpc ? "some cores have v8.1+ features (unexpected for Kryo 280)"
		    : "ARMv8.0 (no LSE/PAN/RCpc): topaz patch 0081 applies");
}

/* ---- page 2: memory map from the DTB ABL passed ---- */

struct mem_ctx {
	int in_rsv, lines, hidden;
};

static int starts(const char *s, const char *pre)
{
	while (*pre)
		if (*s++ != *pre++)
			return 0;
	return 1;
}

static void mem_cb(void *vctx, int depth, const char *node, const char *prop, uintptr_t val, uint32_t len)
{
	struct mem_ctx *c = vctx;

	if (!prop) {
		if (depth == 2)
			c->in_rsv = starts(node, "reserved-memory");
		return;
	}
	int is_mem = depth == 2 && starts(node, "memory");
	int is_rsv = depth == 3 && c->in_rsv;
	int is_rd = depth == 2 && starts(node, "chosen") && starts(prop, "linux,initrd-");
	if (!((is_mem || is_rsv) && starts(prop, "reg") && prop[3] == 0) && !is_rd)
		return;

	for (uint32_t off = 0; off < len; off += 16) {
		char b[72], *p = b;
		if (c->lines >= 46) {
			c->hidden++;
			return;
		}
		if (is_rd) {
			p = st(p, prop + 13);	/* "start" / "end" */
			p = st(p, " ");
			uint64_t v = len == 8 ? (uint64_t)fdt_be32(val) << 32 | fdt_be32(val + 4) : fdt_be32(val);
			hx(p, v, 9);
			con_str("INITRD", b);
			c->lines++;
			return;
		}
		if (len - off < 16) {	/* not 2+2 cells */
			p = kv(p, "reg len=", len, 4);
			con_str("", b);
			c->lines++;
			return;
		}
		uint64_t base = (uint64_t)fdt_be32(val + off) << 32 | fdt_be32(val + off + 4);
		uint64_t size = (uint64_t)fdt_be32(val + off + 8) << 32 | fdt_be32(val + off + 12);
		int n = 0;
		if (is_rsv)
			for (const char *s = node; *s && n < 26; s++, n++)
				*p++ = *s;
		for (; n < 27; n++)
			*p++ = ' ';
		p = kv(p, "", base, 9);
		hx(p, size, 9);
		con_str(is_mem ? "MEMORY" : "", b);
		c->lines++;
	}
}

void probe_mem(uintptr_t dtb)
{
	struct mem_ctx c = { 0, 0, 0 };

	con_str("", "                           base      size");
	if (!fdt_walk(dtb, mem_cb, &c)) {
		con_hex("BAD DTB", dtb);
		return;
	}
	if (c.hidden) {
		char b[32], *p = b;
		p = dec(p, c.hidden);
		st(p, " more not shown");
		con_str("", b);
	}
}

/* ---- page 3: PMIC (PM8998 on SPMI, arbiter v3), touch clocks and pins ---- */

#define SPMI_CORE	0x0800f000UL	/* "core": apid map at +0x800 */
#define SPMI_OBS	0x09400000UL	/* "obsrvr": EE 0, 0x8000 per apid */
#define SPMI_CNFG	0x0800a000UL	/* "cnfg": owner at +0x700 */
#define SPMI_APIDS	512

static int spmi_apid(uint16_t ppid, uint32_t *owner)
{
	int found = -1;
	for (int n = 0; n < SPMI_APIDS; n++) {
		uint32_t map = rd32(SPMI_CORE + 0x800 + 4 * n);
		if (!map || ((map >> 8) & 0xfff) != ppid)
			continue;
		uint32_t own = rd32(SPMI_CNFG + 0x700 + 4 * n) & 7;
		if (found < 0 || own == 0) {
			found = n;
			*owner = own;
		}
	}
	return found;
}

/* One byte through the observer channel; <0 on error (status bits 1-3: failed/denied/dropped). */
static int spmi_rd(int apid, uint8_t addr)
{
	if (apid < 0)
		return -1;
	uintptr_t ch = SPMI_OBS + 0x8000UL * apid;
	__asm__ volatile("dmb oshst" ::: "memory");
	wr32(ch, (1u << 27) | ((uint32_t)addr << 4));	/* EXT_READL, 1 byte */
	__asm__ volatile("dsb osh" ::: "memory");
	for (int i = 0; i < 1000; i++) {
		uint32_t s = rd32(ch + 0x08);
		if (s & 1)
			return (s & 0xe) ? -(int)s : (int)(rd32(ch + 0x18) & 0xff);
		delay_us(1);
	}
	return -2;
}

static char *reg(char *p, const char *k, int v)
{
	p = st(p, k);
	if (v < 0)
		return st(p, v == -2 ? "tmo " : "err ");
	p = hx(p, v, 2);
	return st(p, " ");
}

struct periph {
	uint16_t ppid;
	char name[7];
};

static int apid_pon = -1, apid_g5 = -1, apid_g6 = -1;

void probe_pmic(void)
{
	char b[72], *p;
	uint32_t own = 0;

	con_hex2("SPMI", "ver=", rd32(SPMI_CORE), 0, 0);

	int a = spmi_apid(0x001, &own);	/* PM8998 REVID */
	p = b;
	p = kv(p, "apid=", a, 3);
	p = reg(p, "type=", spmi_rd(a, 0x04));
	p = reg(p, "sub=", spmi_rd(a, 0x05));
	reg(p, "rev4=", spmi_rd(a, 0x03));
	con_str("REVID", b);

	apid_pon = spmi_apid(0x008, &own);	/* PON: power key RT bit 0, RESIN bit 1 */
	p = b;
	p = kv(p, "apid=", apid_pon, 3);
	reg(p, "rt=", spmi_rd(apid_pon, 0x10));
	con_str("PON", b);

	/* PM8998 GPIO5 = volume down, GPIO6 = volume up (active low, pull-up) */
	apid_g5 = spmi_apid(0x0c4, &own);
	apid_g6 = spmi_apid(0x0c5, &own);
	for (int g = 0; g < 2; g++) {
		int ap = g ? apid_g6 : apid_g5;
		p = b;
		p = kv(p, "apid=", ap, 3);
		p = reg(p, "st1=", spmi_rd(ap, 0x08));
		p = reg(p, "rt=", spmi_rd(ap, 0x10));
		p = reg(p, "mode=", spmi_rd(ap, 0x40));
		reg(p, "en=", spmi_rd(ap, 0x46));
		con_str(g ? "VOL+ g6" : "VOL- g5", b);
	}

	/*
	 * Regulators the first drivers need, all owned by RPM. L28 3.0V and L6
	 * 1.8V feed the Synaptics touch IC, L20/L26 the UFS. en bit 7 = enabled;
	 * 0x40/0x41 are the voltage registers, raw (their meaning depends on
	 * the LDO subtype).
	 */
	static const struct periph ldo[] = {
		{ 0x05b, "L28 tp" }, { 0x045, "L6 tp" }, { 0x053, "L20" }, { 0x059, "L26" },
	};
	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		a = spmi_apid(ldo[i].ppid, &own);
		p = b;
		p = kv(p, "apid=", a, 3);
		p = kv(p, "own=", own, 1);
		p = reg(p, "st=", spmi_rd(a, 0x08));
		p = reg(p, "en=", spmi_rd(a, 0x46));
		p = reg(p, "v40=", spmi_rd(a, 0x40));
		reg(p, "v41=", spmi_rd(a, 0x41));
		con_str(ldo[i].name, b);
	}

	/* GCC branch clocks (bit 31 CLK_OFF): touch I2C (BLSP1 QUP5) and UFS */
	const uintptr_t gcc = 0x00100000;
	uint32_t ahb = rd32(gcc + 0x17004), qup = rd32(gcc + 0x21008);
	con_hex2("GCC", "blsp1=", ahb, "qup5=", qup);
	con_hex2("", "ufsahb=", rd32(gcc + 0x7500c), "ufsaxi=", rd32(gcc + 0x75008));

	/* QUP5 registers are only safe to touch with both clocks running */
	if (!(ahb >> 31) && !(qup >> 31)) {
		const uintptr_t q = 0x0c179000;
		con_hex2("QUP5", "hw=", rd32(q + 0x30), "state=", rd32(q + 0x04));
		con_hex2("", "i2cclk=", rd32(q + 0x400), "i2cst=", rd32(q + 0x404));
	} else {
		con_str("QUP5", "clock off: touch driver must enable it");
	}

	/* TLMM EAST tile: I2C5 SDA/SCL (func 3 = blsp_i2c5), touch reset and irq */
	static const struct periph pins[] = {
		{ 87, "sda 87" }, { 88, "scl 88" }, { 89, "rst 89" }, { 125, "irq125" },
	};
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uintptr_t t = 0x03400000UL + 0x900000UL + 0x1000UL * pins[i].ppid;
		uint32_t cfg = rd32(t), io = rd32(t + 4);
		p = b;
		p = kv(p, "func=", (cfg >> 2) & 0xf, 1);
		p = kv(p, "drv=", (cfg >> 6) & 7, 1);
		p = kv(p, "pull=", cfg & 3, 1);
		p = kv(p, "oe=", (cfg >> 9) & 1, 1);
		p = kv(p, "in=", io & 1, 1);
		kv(p, "out=", (io >> 1) & 1, 1);
		con_str(pins[i].name, b);
	}
}

void probe_keys_live(unsigned seconds)
{
	uint64_t end = now() + rsr("cntfrq_el0") * seconds;

	con_skip();
	con_str("KEYS", "press power, vol+, vol- now:");
	int r = con_row_get();
	uint64_t t0 = now();
	while (now() < end) {
		char b[72], *p = b;
		con_row_set(r);
		p = st(p, "t=");
		p = dec(p, (now() - t0) / rsr("cntfrq_el0"));
		p = st(p, "s ");
		p = reg(p, "pon=", spmi_rd(apid_pon, 0x10));
		p = reg(p, "vol-=", spmi_rd(apid_g5, 0x08));
		reg(p, "vol+=", spmi_rd(apid_g6, 0x08));
		con_str("", b);
		delay_us(100000);
	}
}

/* ---- CPU clocks: OSM (per-cluster DCVS sequencer), cluster PLLs, ACD muxes ---- */

/*
 * MSM8998 CPU clocks (downstream drivers/clk/msm/clock-osm.c, DT node
 * qcom,cpu-clock-8998@179c0000): one OSM block per cluster, silver at
 * 0x179c0000, gold at +0x2000. The Linux driver loads the OSM sequencer,
 * its frequency/voltage table and enables it; a frequency change is then a
 * table index written to DCVS_PERF_STATE_DESIRED. Read-only here.
 */
#define OSM_CL(c)		(0x179c0000UL + 0x2000UL * (c))
#define OSM_ENABLE		0x1004
#define OSM_CYC_CTRL		0x1f00
#define OSM_CYC_STATUS		0x1f04
#define OSM_DESIRED		0x1f10
#define OSM_PSTATE		0x1c00

static const uintptr_t cl_pll[2] = { 0x17916000, 0x17816000 };	/* pwrcl, perfcl */
static const uintptr_t cl_acd[2] = { 0x17914800, 0x17814800 };

/* Cluster clock in MHz from the OSM cycle counter over 10 ms; 0 if it does not count. */
static uint32_t osm_mhz(int c)
{
	uint32_t a = rd32(OSM_CL(c) + OSM_CYC_STATUS);
	delay_us(10000);
	uint32_t b = rd32(OSM_CL(c) + OSM_CYC_STATUS);
	return (b - a) / 10000;
}

void probe_osm(void)
{
	for (int c = 0; c < 2; c++) {
		uintptr_t o = OSM_CL(c);
		con_hex2(c ? "OSM gold" : "OSM silv", "ver=", rd32(o), "en=", rd32(o + OSM_ENABLE));
		con_hex2("", "desired=", rd32(o + OSM_DESIRED), "pstate=", rd32(o + OSM_PSTATE));
		uint32_t mhz = osm_mhz(c);
		con_hex2("", "cycctl=", rd32(o + OSM_CYC_CTRL), "mhz=", mhz);
		con_hex2(c ? "PLL gold" : "PLL silv", "mode=", rd32(cl_pll[c]), "l=", rd32(cl_pll[c] + 4));
		con_hex2("", "user=", rd32(cl_pll[c] + 0xc), "status=", rd32(cl_pll[c] + 0x2c));
		con_hex2(c ? "ACD gold" : "ACD silv", "ver=", rd32(cl_acd[c]), "gfmux=", rd32(cl_acd[c] + 0x3c));
	}
}

int probe_osm_gold_enabled(void)
{
	return rd32(OSM_CL(1) + OSM_ENABLE) & 1;
}

uint32_t probe_osm_gold_desired(void)
{
	return rd32(OSM_CL(1) + OSM_DESIRED);
}

/* Ask the gold OSM for table entry idx (it sequences PLL, mux and voltage itself). */
void probe_osm_gold_request(uint32_t idx)
{
	wr32(OSM_CL(1) + OSM_DESIRED, idx);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_us(5000);
}

uint32_t probe_osm_gold_mhz(void)
{
	return osm_mhz(1);
}
