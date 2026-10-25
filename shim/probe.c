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

/* ---- touch: RPM rail vote, BLSP1 QUP5 I2C, Synaptics RMI4 (S3320 @0x20) ---- */

/*
 * Everything the touch IC needs that ABL leaves off, done the way mainline
 * Linux does it (drivers/rpmsg/qcom_glink_{rpm,native}.c, soc/qcom/smd-rpm.c,
 * regulator/qcom_smd-regulator.c, clk/qcom/gcc-msm8998.c, i2c/busses/
 * i2c-qup.c, input/rmi4). Writes: RPM message RAM FIFOs and the APCS IPC
 * doorbell, GCC BLSP1 AHB vote and QUP5 I2C branch, TLMM 87/88/89/125, QUP5.
 */
#define RPM_MSGRAM	0x00778000UL	/* qcom,rpm-msg-ram */
#define RPM_MSGRAM_SZ	0x7000
#define APCS_IPC	0x17911008UL	/* msm8998 apcs-hmss-global: offset 8, bit 0 = RPM */

struct gpipe {
	uintptr_t tail, head, fifo;
	uint32_t len;
};
static struct gpipe grx, gtx;
static uint8_t gdata[64];	/* last TX_DATA payload from the RPM */
static uint32_t gdata_len, g_rcid, g_lcid_ack, g_vack, g_ropen;
static char glog[40];		/* received command numbers, as hex digits */
static int glog_n;

static int glink_toc(char *b)
{
	uintptr_t toc = RPM_MSGRAM + RPM_MSGRAM_SZ - 256;
	char *p = b;
	if (rd32(toc) != 0x67727430) {	/* "grt0" */
		kv(p, "bad magic ", rd32(toc), 8);
		return -1;
	}
	uint32_t n = rd32(toc + 4);
	if (n > 20)
		return -1;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t id = rd32(toc + 8 + 12 * i), off = rd32(toc + 12 + 12 * i), sz = rd32(toc + 16 + 12 * i);
		if (off + sz > RPM_MSGRAM_SZ)
			continue;
		struct gpipe *g = id == 0x72326170 ? &grx : id == 0x61703272 ? &gtx : 0;	/* r2ap, ap2r */
		if (!g)
			continue;
		g->tail = RPM_MSGRAM + off;
		g->head = RPM_MSGRAM + off + 4;
		g->fifo = RPM_MSGRAM + off + 8;
		g->len = sz;
	}
	p = kv(p, "n=", n, 1);
	p = kv(p, "rx=", grx.tail - RPM_MSGRAM, 4);
	p = kv(p, "/", grx.len, 4);
	p = kv(p, "tx=", gtx.tail - RPM_MSGRAM, 4);
	kv(p, "/", gtx.len, 4);
	return grx.len && gtx.len ? 0 : -1;
}

static uint32_t g_wrap(uint32_t v, uint32_t len)
{
	return v >= len ? v - len : v;
}

/* nwords 32-bit words (a multiple of 2: GLINK pads to 8 bytes), then kick. */
static int glink_tx(const uint32_t *w, unsigned nwords)
{
	uint32_t head = rd32(gtx.head), tail = rd32(gtx.tail);
	uint32_t avail = tail <= head ? gtx.len - head + tail : tail - head;
	if (head >= gtx.len || avail <= nwords * 4)
		return -1;
	for (unsigned i = 0; i < nwords; i++) {
		wr32(gtx.fifo + head, w[i]);
		head = g_wrap(head + 4, gtx.len);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(gtx.head, head);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(APCS_IPC, 1);
	return 0;
}

static uint32_t grx_avail(void)
{
	uint32_t head = rd32(grx.head), tail = rd32(grx.tail);
	if (head >= grx.len || tail >= grx.len)
		return 0;
	return head < tail ? grx.len - tail + head : head - tail;
}

static uint32_t grx_word(uint32_t off)
{
	return rd32(grx.fifo + g_wrap(rd32(grx.tail) + off, grx.len));
}

static void grx_advance(uint32_t n)
{
	wr32(grx.tail, g_wrap(rd32(grx.tail) + ((n + 7) & ~7u), grx.len));
}

static void glink_cmd(uint16_t cmd, uint16_t p1, uint32_t p2)
{
	uint32_t w[2] = { cmd | (uint32_t)p1 << 16, p2 };
	glink_tx(w, 2);
}

/* Handle everything the RPM has sent; returns <0 on an unknown command. */
static int glink_rx(void)
{
	while (grx_avail() >= 8) {
		uint32_t w0 = grx_word(0), p2 = grx_word(4);
		uint32_t cmd = w0 & 0xffff, p1 = w0 >> 16, len = 8;
		if (glog_n < (int)sizeof(glog) - 1)
			glog[glog_n++] = "0123456789ABCDEF"[cmd & 0xf];
		glog[glog_n] = 0;
		switch (cmd) {
		case 0:		/* VERSION: ack with ours, no features (intentless RPM link) */
			glink_cmd(1, 1, 0);
			break;
		case 1:		/* VERSION_ACK */
			g_vack = 1;
			break;
		case 2:		/* OPEN from the RPM: name follows */
			g_rcid = p1;
			g_ropen = 1;
			len = 8 + (p2 & 0xffff);
			glink_cmd(4, p1, 0);	/* OPEN_ACK */
			break;
		case 4:		/* OPEN_ACK for our lcid */
			g_lcid_ack = p1;
			break;
		case 5:		/* INTENT: p2 pairs of {size, iid} */
			len = 8 + 8 * p2;
			break;
		case 9:
		case 12: {	/* TX_DATA(_CONT): chunk_size, left_size, data */
			uint32_t chunk = grx_word(8);
			if (chunk > 1024)
				return -1;
			for (uint32_t i = 0; i < chunk && i < sizeof(gdata); i += 4) {
				uint32_t v = grx_word(16 + i);
				for (int k = 0; k < 4 && i + k < sizeof(gdata); k++)
					gdata[i + k] = v >> (8 * k);
			}
			gdata_len = chunk;
			len = 16 + chunk;
			break;
		}
		case 3:		/* CLOSE */
			glink_cmd(11, p1, 0);
			break;
		case 6: case 7: case 8: case 11: case 13: case 14: case 15:
			break;
		default:
			return -1;
		}
		grx_advance(len);
	}
	return 0;
}

static int glink_wait(volatile uint32_t *flag, unsigned ms)
{
	for (unsigned i = 0; i < ms * 10; i++) {
		if (glink_rx() < 0)
			return -1;
		if (*flag)
			return 0;
		delay_us(100);
	}
	return -2;
}

/* qcom_rpm_smd_write: one LDO-A resource, active set, swen=1 and uv. */
static uint32_t rpm_msg_id = 1;
static int rpm_ldo_on(uint32_t ldo, uint32_t uv, char *b)
{
	uint32_t w[18];
	unsigned n = 0;
	w[n++] = 9;			/* TX_DATA, lcid 1 (param1 << 16) */
	w[n - 1] |= 1u << 16;
	w[n++] = 0;			/* iid: intentless */
	w[n++] = 52;			/* chunk_size */
	w[n++] = 0;			/* left_size */
	w[n++] = 0x00716572;		/* "req" */
	w[n++] = 20 + 24;
	w[n++] = rpm_msg_id++;
	w[n++] = 0;			/* active state */
	w[n++] = 0x616f646c;		/* "ldoa" */
	w[n++] = ldo;
	w[n++] = 24;
	w[n++] = 0x6e657773; w[n++] = 4; w[n++] = 1;	/* swen */
	w[n++] = 0x00007675; w[n++] = 4; w[n++] = uv;	/* uv */
	w[n++] = 0;			/* pad 68 -> 72 bytes */
	gdata_len = 0;
	char *p = b;
	if (glink_tx(w, n) < 0)
		return (void)st(p, "tx full"), -1;
	for (int i = 0; i < 10000 && !gdata_len; i++) {
		if (glink_rx() < 0)
			return (void)st(p, "rx bad cmd"), -1;
		delay_us(100);
	}
	if (!gdata_len)
		return (void)st(p, "no answer"), -1;
	/* answer: "req" len, then messages {type, len, ...}: "msg#" id or "err" text */
	uint32_t t = gdata_len >= 12 ? gdata[8] | gdata[9] << 8 | gdata[10] << 16 | (uint32_t)gdata[11] << 24 : 0;
	if (t == 0x00727265) {
		p = st(p, "err ");
		for (uint32_t i = 16; i < gdata_len && i < 40 && gdata[i]; i++)
			*p++ = gdata[i];
		*p = 0;
		return -1;
	}
	p = kv(p, "ans=", t, 8);
	kv(p, "len=", gdata_len, 2);
	return t == 0x2367736d ? 0 : -1;	/* "msg#" */
}

static int ldo_status(uint16_t ppid, char *b)
{
	uint32_t own = 0;
	int a = spmi_apid(ppid, &own);
	char *p = b;
	p = kv(p, "apid=", a, 3);
	p = reg(p, "st=", spmi_rd(a, 0x08));
	reg(p, "en=", spmi_rd(a, 0x46));
	return a;
}

/* GCC: BLSP1 AHB (voted, APCS vote register) and the QUP5 I2C core clock. */
#define GCC		0x00100000UL
static int gcc_on(char *b)
{
	wr32(GCC + 0x52004, rd32(GCC + 0x52004) | (1u << 17));
	wr32(GCC + 0x21024, 0);			/* QUP5 I2C RCG: XO 19.2 MHz, no divider */
	wr32(GCC + 0x21020, rd32(GCC + 0x21020) | 1);	/* UPDATE */
	for (int i = 0; i < 1000 && (rd32(GCC + 0x21020) & 1); i++)
		delay_us(1);
	wr32(GCC + 0x21008, rd32(GCC + 0x21008) | 1);
	int ok = 0;
	for (int i = 0; i < 1000; i++) {
		ok = !(rd32(GCC + 0x17004) >> 31) && !(rd32(GCC + 0x21008) >> 31);
		if (ok)
			break;
		delay_us(1);
	}
	char *p = b;
	p = kv(p, "ahb=", rd32(GCC + 0x17004), 8);
	p = kv(p, "qup=", rd32(GCC + 0x21008), 8);
	kv(p, "rcg=", rd32(GCC + 0x21020), 8);
	return ok ? 0 : -1;
}

/* TLMM (EAST tile): cfg bits 1:0 pull (3 up), 5:2 func, 8:6 drive (mA/2-1), 9 OE; io bit 0 in, 1 out */
#define TLMM_PIN(n)	(0x03400000UL + 0x900000UL + 0x1000UL * (n))
static void tlmm_setup(void)
{
	wr32(TLMM_PIN(87), 3 << 2);			/* blsp_i2c5 SDA, 2 mA, no pull */
	wr32(TLMM_PIN(88), 3 << 2);			/* blsp_i2c5 SCL */
	wr32(TLMM_PIN(125), (3 << 6) | 3);		/* touch irq: gpio in, 8 mA, pull-up */
	wr32(TLMM_PIN(89) + 4, 0);			/* reset: low until the rails are up */
	wr32(TLMM_PIN(89), (1 << 9) | (3 << 6) | 3);	/* gpio out */
}

/*
 * Bit-banged I2C on the same pins (func 0, open drain by toggling OE with
 * out=0; the board pull-ups go to L6), ~100 kHz. Only to tell a QUP setup
 * problem from a silent chip: returns 1 if addr ACKs a read address byte.
 */
#define BB_SDA	TLMM_PIN(87)
#define BB_SCL	TLMM_PIN(88)
static void bb_line(uintptr_t pin, int high)
{
	wr32(pin, high ? 0 : 1 << 9);
	delay_us(5);
}

static int bb_probe(uint8_t addr)
{
	wr32(BB_SDA + 4, 0);
	wr32(BB_SCL + 4, 0);
	bb_line(BB_SDA, 1);
	bb_line(BB_SCL, 1);
	bb_line(BB_SDA, 0);	/* START */
	bb_line(BB_SCL, 0);
	uint8_t v = addr << 1 | 1;
	for (int i = 7; i >= 0; i--) {
		bb_line(BB_SDA, (v >> i) & 1);
		bb_line(BB_SCL, 1);
		bb_line(BB_SCL, 0);
	}
	bb_line(BB_SDA, 1);
	bb_line(BB_SCL, 1);
	int ack = !(rd32(BB_SDA + 4) & 1);
	bb_line(BB_SCL, 0);
	if (ack) {	/* clock one byte out of the slave, NACK it */
		for (int i = 0; i < 9; i++) {
			bb_line(BB_SCL, 1);
			bb_line(BB_SCL, 0);
		}
	}
	bb_line(BB_SDA, 0);
	bb_line(BB_SCL, 1);
	bb_line(BB_SDA, 1);	/* STOP */
	return ack;
}

/* Line levels with every driver released: both should read 1 with L6 up. */
static int bb_idle(void)
{
	wr32(BB_SDA, 0);
	wr32(BB_SCL, 0);
	delay_us(20);
	return (rd32(BB_SDA + 4) & 1) | (rd32(BB_SCL + 4) & 1) << 1;
}

static int ts_irq(void)
{
	return rd32(TLMM_PIN(125) + 4) & 1;
}

/* ---- QUP v2 I2C, FIFO mode, polled; one message per call like i2c-qup.c ---- */
#define QUP		0x0c179000UL
#define QUP_CONFIG	0x000
#define QUP_STATE	0x004
#define QUP_IO_MODE	0x008
#define QUP_SW_RESET	0x00c
#define QUP_OPERATIONAL	0x018
#define QUP_ERROR_FLAGS	0x01c
#define QUP_MX_OUT_CNT	0x100
#define QUP_OUT_FIFO	0x110
#define QUP_MX_WR_CNT	0x150
#define QUP_MX_IN_CNT	0x200
#define QUP_MX_RD_CNT	0x208
#define QUP_IN_FIFO	0x218
#define QUP_I2C_CLK	0x400
#define QUP_I2C_STATUS	0x404
#define QUP_MASTER_GEN	0x408
#define QUP_CLK_100K	((3 << 8) | 93)		/* hs_div 3, fs_div 19.2M/100k/2-3 */

static uint32_t qup_last_st, qup_last_err, qup_rxtag;

static int qup_state(uint32_t want, uint32_t mask)
{
	for (int i = 0; i < 1000; i++) {
		uint32_t s = rd32(QUP + QUP_STATE);
		if ((s & 4) && (s & mask) == want)
			return 0;
		delay_us(1);
	}
	return -1;
}

static int qup_go(uint32_t st)
{
	if (qup_state(0, 0))
		return -1;
	wr32(QUP + QUP_STATE, st);
	return qup_state(st, 3);
}

/*
 * rd=0: write n bytes from buf; rd=1: read n bytes (n <= 32) into buf.
 * Returns 0, -1 controller error, -2 timeout, -3 NACK (bus error flags).
 */
static int qup_xfer(uint8_t addr, int rd, uint8_t *buf, unsigned n)
{
	uint8_t tx[40];
	unsigned ntx = 0, nrx = rd ? 2 + n : 0;
	tx[ntx++] = 0x81;			/* START */
	tx[ntx++] = addr << 1 | rd;
	tx[ntx++] = rd ? 0x87 : 0x83;		/* DATARD_STOP / DATAWR_STOP */
	tx[ntx++] = n;
	if (!rd)
		for (unsigned i = 0; i < n; i++)
			tx[ntx++] = buf[i];

	wr32(QUP + QUP_SW_RESET, 1);
	if (qup_state(0, 3))
		return -1;
	wr32(QUP + QUP_CONFIG, (2 << 8) | 7);
	wr32(QUP + QUP_MASTER_GEN, 1);		/* v2 tags */
	if (qup_state(0x10, 0x10))
		return -1;
	wr32(QUP + QUP_IO_MODE, 3 << 14);	/* FIFO in and out, pack/unpack */
	wr32(QUP + QUP_MX_OUT_CNT, 0);
	wr32(QUP + QUP_MX_IN_CNT, 0);
	wr32(QUP + QUP_MX_WR_CNT, ntx);
	if (nrx)
		wr32(QUP + QUP_MX_RD_CNT, nrx);
	wr32(QUP + QUP_CONFIG, (2 << 8) | 7 | (nrx ? 0 : 1 << 7));
	if (qup_go(1))
		return -1;
	wr32(QUP + QUP_I2C_CLK, QUP_CLK_100K);
	if (qup_go(3))
		return -1;
	for (unsigned i = 0; i < ntx; i += 4) {
		uint32_t w = 0;
		for (unsigned k = 0; k < 4 && i + k < ntx; k++)
			w |= (uint32_t)tx[i + k] << (8 * k);
		wr32(QUP + QUP_OUT_FIFO, w);
	}
	if (qup_go(1))
		return -1;

	uint8_t rx[40];
	unsigned got = 0;
	int ret = -2;
	uint64_t t0 = now(), end = t0 + rsr("cntfrq_el0") / 50;	/* 20 ms */
	while (now() < end) {
		uint32_t s = rd32(QUP + QUP_I2C_STATUS), e = rd32(QUP + QUP_ERROR_FLAGS);
		if ((s & 0x38000fc) || (e & 0x7c)) {
			qup_last_st = s;
			qup_last_err = e;
			ret = (s & 0x8) ? -3 : -1;
			break;
		}
		uint32_t op = rd32(QUP + QUP_OPERATIONAL);
		while ((op & (1 << 5)) && got < nrx) {
			uint32_t w = rd32(QUP + QUP_IN_FIFO);
			for (int k = 0; k < 4 && got < nrx; k++)
				rx[got++] = w >> (8 * k);
			op = rd32(QUP + QUP_OPERATIONAL);
		}
		if (nrx ? got >= nrx : !(op & (1 << 4)) && !(s & (1 << 8)) && now() - t0 > rsr("cntfrq_el0") / 5000) {
			ret = 0;
			break;
		}
	}
	for (int i = 0; i < 1000 && (rd32(QUP + QUP_I2C_STATUS) & (1 << 8)); i++)
		delay_us(1);
	if (!ret) {
		uint32_t s = rd32(QUP + QUP_I2C_STATUS);
		if (s & 0x38000fc) {
			qup_last_st = s;
			ret = (s & 0x8) ? -3 : -1;
		}
	}
	qup_go(0);
	if (!ret && rd) {
		qup_rxtag = rx[0] | rx[1] << 8;
		for (unsigned i = 0; i < n; i++)
			buf[i] = rx[2 + i];
	}
	return ret;
}

/* RMI4 over I2C (rmi_i2c.c): 0xff selects the page, then a 1-byte register address. */
static uint8_t rmi_addr = 0x20;
static int rmi_page = -1;

static int rmi_set_page(int page)
{
	if (page == rmi_page)
		return 0;
	uint8_t b[2] = { 0xff, (uint8_t)page };
	int r = qup_xfer(rmi_addr, 0, b, 2);
	rmi_page = r ? -1 : page;
	return r;
}

static int rmi_read(uint16_t a, uint8_t *buf, unsigned n)
{
	int r = rmi_set_page(a >> 8);
	if (r)
		return r;
	uint8_t reg8 = a & 0xff;
	r = qup_xfer(rmi_addr, 0, &reg8, 1);
	return r ? r : qup_xfer(rmi_addr, 1, buf, n);
}

struct rmi_fn {
	uint8_t num, irqs;
	uint16_t query, ctrl, data;
};
static struct rmi_fn rmi_fns[8];
static int rmi_nfn;

static char *rerr(char *p, int r)
{
	return st(p, r == -3 ? "NACK" : r == -2 ? "timeout" : "ctrl err");
}

static void rmi_scan(void)
{
	char b[72], *p;
	rmi_nfn = 0;
	for (int page = 0; page < 4; page++) {
		for (int a = 0xe9; a > 0x0a; a -= 6) {
			uint8_t e[6];
			int r = rmi_read(page << 8 | a, e, 6);
			if (r) {
				rerr(st(b, "pdt read: "), r);
				con_str("RMI", b);
				return;
			}
			if (!e[5] || e[5] == 0xff)
				break;
			if (rmi_nfn < 8) {
				struct rmi_fn *f = &rmi_fns[rmi_nfn++];
				f->num = e[5];
				f->irqs = e[4] & 7;
				f->query = page << 8 | e[0];
				f->ctrl = page << 8 | e[2];
				f->data = page << 8 | e[3];
			}
			p = b;
			p = kv(p, "F", e[5], 2);
			p = kv(p, "pg=", page, 1);
			p = kv(p, "q=", e[0], 2);
			p = kv(p, "c=", e[1], 2);
			p = kv(p, "ctl=", e[2], 2);
			p = kv(p, "d=", e[3], 2);
			kv(p, "irq=", e[4], 2);
			con_str("PDT", b);
		}
	}
	if (!rmi_nfn)
		con_str("RMI", "no functions in the PDT");
}

static struct rmi_fn *rmi_find(uint8_t num)
{
	for (int i = 0; i < rmi_nfn; i++)
		if (rmi_fns[i].num == num)
			return &rmi_fns[i];
	return 0;
}

void probe_touch(unsigned live_s)
{
	char b[72], *p;

	/* 1. bus before power: a NACK here proves the controller and pins work */
	if (gcc_on(b) < 0) {
		con_str("GCC", b);
		con_str("GCC", "clocks did not start: stop");
		return;
	}
	con_str("GCC", b);
	tlmm_setup();
	con_hex2("TLMM", "sda=", rd32(TLMM_PIN(87)), "irq=", rd32(TLMM_PIN(125)));
	con_hex2("QUP5", "hw=", rd32(QUP + 0x30), "io=", rd32(QUP + QUP_IO_MODE));
	uint8_t pg[2] = { 0xff, 0 };
	int r = qup_xfer(rmi_addr, 0, pg, 2);
	p = st(b, "unpowered @20: ");
	p = r ? rerr(p, r) : st(p, "ACK (rail already on?)");
	kv(p, " st=", qup_last_st, 8);
	con_str("I2C", b);
	ldo_status(0x145, b);
	con_str("L6 sid1", b);
	ldo_status(0x15b, b);
	con_str("L28 sid1", b);
	con_skip();

	/* 2. RPM: GLINK handshake on the RPM edge, open rpm_requests */
	if (glink_toc(b) < 0) {
		con_str("GLINK", b);
		con_str("GLINK", "no RPM TOC: stop");
		return;
	}
	con_str("TOC", b);
	p = b;
	p = kv(p, "rx t/h=", rd32(grx.tail), 4);
	p = kv(p, "/", rd32(grx.head), 4);
	p = kv(p, "tx t/h=", rd32(gtx.tail), 4);
	kv(p, "/", rd32(gtx.head), 4);
	con_str("FIFOS", b);
	wr32(gtx.head, 0);	/* as glink_rpm_probe: our ends of both pipes */
	wr32(grx.tail, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	glink_cmd(0, 1, 0);	/* VERSION 1, no features */
	r = glink_wait(&g_vack, 1000);
	p = st(b, r ? "no VERSION_ACK " : "VERSION ok ");
	st(st(p, "rx="), glog);
	con_str("GLINK", b);
	if (r)
		return;
	glink_wait(&g_ropen, 300);	/* the RPM may open rpm_requests first */
	{
		static const char name[] = "rpm_requests";	/* 13 bytes with NUL */
		uint32_t w[6] = { 2 | 1u << 16, sizeof(name) };	/* OPEN lcid 1 */
		for (unsigned i = 0; i < sizeof(name); i++)
			w[2 + i / 4] |= (uint32_t)(uint8_t)name[i] << (8 * (i % 4));
		glink_tx(w, 6);	/* 8 + 13 -> 24 bytes */
	}
	r = glink_wait(&g_lcid_ack, 1000);
	if (!r && !g_ropen)
		glink_wait(&g_ropen, 500);
	p = st(b, r ? "no OPEN_ACK " : "open ok ");
	p = kv(p, "rcid=", g_rcid, 2);
	st(st(p, "rx="), glog);
	con_str("GLINK", b);
	if (r)
		return;

	/* 3. touch rails as tpd_hw_pwron (OnePlus s3320 driver): L28 3.0 V, 10 ms, L6 1.8 V, 10 ms */
	r = rpm_ldo_on(28, 3008000, b);
	con_str("RPM L28", b);
	delay_us(10000);
	r |= rpm_ldo_on(6, 1808000, b);
	con_str("RPM L6", b);
	delay_us(10000);
	ldo_status(0x15b, b);
	con_str("L28 sid1", b);
	ldo_status(0x145, b);
	con_str("L6 sid1", b);

	/* 4. reset high 10 ms, low 10 ms, high; then find who answers, for 3 s */
	wr32(TLMM_PIN(89) + 4, 2);
	delay_us(10000);
	wr32(TLMM_PIN(89) + 4, 0);
	delay_us(10000);
	wr32(TLMM_PIN(89) + 4, 2);
	con_skip();
	uint64_t tr = now();
	int found = -1;
	for (int pass = 0; pass < 6; pass++) {
		delay_us(pass ? 500000 : 100000);
		p = b;
		p = st(p, "t=");
		p = dec(p, (now() - tr) / (rsr("cntfrq_el0") / 1000));
		p = st(p, "ms irq=");
		p = dec(p, ts_irq());
		p = st(p, " qup:");
		int n = 0;
		uint32_t st0 = 0;
		for (int a = 0x08; a < 0x78; a++) {
			uint8_t d;
			int rr = qup_xfer(a, 1, &d, 1);
			if (a == 0x20)
				st0 = rr ? qup_last_st : 0;
			if (!rr && n++ < 4) {
				p = hx(st(p, " "), a, 2);
				if (found < 0 || a == 0x20)
					found = a;
			}
		}
		if (!n)
			p = kv(p, " none st20=", st0, 8);
		con_str(pass ? "" : "SCAN", b);
	}
	/* same scan bit-banged: a silent QUP but an ACK here means the QUP setup is wrong */
	{
		p = b;
		p = kv(p, "idle sda|scl<<1=", bb_idle(), 1);
		p = st(p, "ack:");
		int n = 0;
		for (int a = 0x08; a < 0x78; a++)
			if (bb_probe(a) && n++ < 5)
				p = hx(st(p, " "), a, 2);
		if (!n)
			st(p, " none");
		con_str("BITBANG", b);
		wr32(TLMM_PIN(87), 3 << 2);	/* back to blsp_i2c5 */
		wr32(TLMM_PIN(88), 3 << 2);
	}
	if (found >= 0)
		rmi_addr = found;
	rmi_page = -1;
	con_skip();
	rmi_scan();
	struct rmi_fn *f01 = rmi_find(0x01), *f12 = rmi_find(0x12);
	if (f01) {
		uint8_t q[21];
		r = rmi_read(f01->query, q, 21);
		p = b;
		if (r)
			rerr(p, r);
		else {
			p = kv(p, "mfr=", q[0], 2);
			p = st(p, "id=");
			for (int i = 11; i < 21 && q[i]; i++)
				*p++ = (q[i] >= 0x20 && q[i] < 0x7f) ? q[i] : '?';
			*p = 0;
			p = kv(p, " tag=", qup_rxtag, 4);
		}
		con_str("F01", b);
	}
	if (!f12) {
		con_str("F12", "not found: no live view");
		return;
	}

	/* 5. live: irq line, F01 interrupt status (reading clears it), F12 data bytes */
	con_skip();
	p = st(b, "touch the screen for ");
	p = dec(p, live_s);
	st(p, " s");
	con_str("TOUCH", b);
	int row = con_row_get();
	uint64_t t0 = now(), end = t0 + rsr("cntfrq_el0") * live_s;
	unsigned events = 0;
	while (now() < end) {
		uint8_t is = 0, d[12];
		int irq = ts_irq();
		if (!irq)
			events++;
		rmi_read(f01->data + 1, &is, 1);
		r = rmi_read(f12->data, d, 12);
		con_row_set(row);
		p = b;
		p = st(p, "t=");
		p = dec(p, (now() - t0) / rsr("cntfrq_el0"));
		p = st(p, " irq=");
		p = dec(p, irq);
		p = st(p, " n=");
		p = dec(p, events);
		p = kv(p, " is=", is, 2);
		st(p, "      ");
		con_str("", b);
		p = b;
		if (r)
			p = rerr(p, r);
		else
			for (int i = 0; i < 12; i++)
				p = st(hx(p, d[i], 2), " ");
		st(p, "      ");
		con_str("F12 d", b);
		if (!r) {
			/* finger 0 if data1 is first: type, x lo/hi, y lo/hi */
			p = b;
			p = kv(p, "type=", d[0], 2);
			p = st(p, "x=");
			p = dec(p, d[1] | d[2] << 8);
			p = st(p, " y=");
			p = dec(p, d[3] | d[4] << 8);
			st(p, "        ");
			con_str("F12 f0", b);
		}
		delay_us(50000);
	}
}
