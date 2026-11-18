// SPDX-License-Identifier: GPL-2.0+
/*
 * NanoPi R76S lab builds of U-Boot proper, experiment-only, see
 * CONFIG_R76S_LAB: the hybrid build, started by the vendor SPL and BL31 from
 * the first FIT copy in the uboot partition (r76s-hybrid.env), and its
 * variants that the vendor U-Boot boots like a kernel for tests.
 *
 * - The vendor U-Boot clears the start of the ATAGS before it jumps to a
 *   kernel, so its boot script saves them for a test variant first, and
 *   board_early_init_f() puts them back (CONFIG_R76S_LAB_ATAGS_COPY): the DRAM
 *   banks and the OP-TEE memory then come from them as when the vendor SPL
 *   starts the hybrid build (sdram.c).
 * - Every console byte is also written to a persistent RAM zone in the
 *   layout of Linux's earlycon=ramlog (a persistent_ram "DBGC" header of
 *   signature, start and size, then the text). Each run starts the zone
 *   afresh and the kernel appends to it, so it holds the log of one boot
 *   from U-Boot on, readable from Linux or after a warm reset.
 * - "r76s info" prints the DRAM banks, the ramlog state, the eMMC mode and
 *   the watchdog registers; "r76s optee" fails if the OP-TEE memory is in a
 *   DRAM bank, which booting a kernel must not risk; "r76s regs" dumps the
 *   eMMC controller registers that its drivers use (reading a reserved
 *   offset such as 0x50c hangs the bus) and its CRU registers;
 *   "r76s reprobe" probes the eMMC controller again, which
 *   resets it through the CRU, and initializes the card; "r76s readtest"
 *   reads a block range repeatedly and compares checksums.
 */

#include <blk.h>
#include <command.h>
#include <cpu_func.h>
#include <dm.h>
#include <mapmem.h>
#include <mmc.h>
#include <time.h>
#include <version_string.h>
#include <vsprintf.h>
#include <asm/arch-rockchip/cru_rk3576.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <dm/device-internal.h>
#include <linux/compiler.h>
#include <linux/sizes.h>
#include <u-boot/crc.h>

DECLARE_GLOBAL_DATA_PTR;

/* ramlog zone, same layout as the kernel's earlycon=ramlog */
#define RAMLOG_SIG	0x43474244	/* "DBGC", PERSISTENT_RAM_SIG */
#define RAMLOG_HDR	12
#define RAMLOG_MAX	(CONFIG_R76S_LAB_RAMLOG_SIZE - RAMLOG_HDR)

/* eMMC controller (sdhci@2a330000) */
#define EMMC_REG(off)	((void __iomem *)(ulong)(0x2a330000 + (off)))

/* Watchdog (watchdog@2ace0000): control, timeout range, current counter */
#define WDT_REG(off)	((void __iomem *)(ulong)(0x2ace0000 + (off)))

/* Where the vendor FIT, and the hybrid FIT built from it, load OP-TEE */
#define OPTEE_BASE	0x48400000

static void ramlog_flush(ulong start, ulong end)
{
	flush_dcache_range(rounddown(start, ARCH_DMA_MINALIGN),
			   roundup(end, ARCH_DMA_MINALIGN));
}

/*
 * Called from puts() and putc() for every console write, from the first one
 * before relocation. Like save_boot_params(), it keeps its only variable in
 * .data, which is usable before relocation and copied by it.
 */
void r76s_ramlog_puts(const char *s)
{
	static bool started __section(".data");
	u32 *hdr = (u32 *)CONFIG_R76S_LAB_RAMLOG_ADDR;
	u8 *text = (u8 *)hdr + RAMLOG_HDR;
	u32 pos, start;

	if (!started) {
		writel(RAMLOG_SIG, &hdr[0]);
		writel(0, &hdr[1]);
		writel(0, &hdr[2]);
		started = true;
	}

	pos = start = readl(&hdr[2]);
	while (*s && pos < RAMLOG_MAX)
		text[pos++] = *s++;
	if (pos == start)
		return;

	ramlog_flush((ulong)text + start, (ulong)text + pos);
	writel(pos, &hdr[2]);
	ramlog_flush((ulong)hdr, (ulong)hdr + RAMLOG_HDR);
}

#if IS_ENABLED(CONFIG_BOARD_EARLY_INIT_F)
/* Put back the ATAGS that the vendor boot script saved (R76S_LAB_ATAGS_COPY) */
int board_early_init_f(void)
{
	if (CONFIG_R76S_LAB_ATAGS_COPY)
		memcpy((void *)(CFG_SYS_SDRAM_BASE + SZ_2M - SZ_8K),
		       (void *)CONFIG_R76S_LAB_ATAGS_COPY, SZ_8K);

	return 0;
}
#endif

static void emmc_dump(const char *when)
{
	/* Only registers that the Linux or vendor driver accesses */
	static const u16 regs[] = {
		0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44,
		0x500, 0x504, 0x508, 0x52c, 0x540,
		0x800, 0x804, 0x808, 0x80c, 0x810, 0x81c, 0x840,
	};
	struct rk3576_cru *cru = (struct rk3576_cru *)RK3576_CRU_BASE;
	int i;

	printf("r76s-lab: eMMC registers %s:\n", when);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		printf("r76s-lab:   %03x: %08x\n", regs[i], readl(EMMC_REG(regs[i])));
	printf("r76s-lab:   CRU clksel89 %08x clksel90 %08x clkgate33 %08x softrst33 %08x\n",
	       readl(&cru->clksel_con[89]), readl(&cru->clksel_con[90]),
	       readl(&cru->clkgate_con[33]), readl(&cru->softrst_con[33]));
}

static void emmc_print(struct mmc *mmc, int ret)
{
	if (ret) {
		printf("r76s-lab: eMMC initialization failed: %d\n", ret);
		return;
	}
	printf("r76s-lab: eMMC %s at %u Hz, HOST_CTRL2 %#06x EMMC_CTRL %#x DLL_STATUS0 %#x DLL_RXCLK %#x\n",
	       mmc_mode_name(mmc->selected_mode), mmc->clock,
	       readw(EMMC_REG(0x3e)), readl(EMMC_REG(0x52c)),
	       readl(EMMC_REG(0x840)), readl(EMMC_REG(0x804)));
	if (mmc->selected_mode != MMC_HS_400_ES)
		printf("r76s-lab: WARNING: the eMMC is not in HS400ES mode\n");
}

static int do_r76s_info(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	const u32 *hdr = (const u32 *)CONFIG_R76S_LAB_RAMLOG_ADDR;
	struct mmc *mmc = find_mmc_device(0);
	int i;

	printf("r76s-lab: %s\n", version_string);
	for (i = 0; i < CONFIG_NR_DRAM_BANKS && gd->dram[i].size; i++)
		printf("r76s-lab: DRAM bank %d: %#llx-%#llx\n", i,
		       (unsigned long long)gd->dram[i].start,
		       (unsigned long long)(gd->dram[i].start +
					    gd->dram[i].size - 1));
	printf("r76s-lab: ramlog at %#x, %u of %u bytes used\n",
	       CONFIG_R76S_LAB_RAMLOG_ADDR, readl(&hdr[2]), RAMLOG_MAX);
	printf("r76s-lab: watchdog CR %#x TORR %#x CCVR %#x\n",
	       readl(WDT_REG(0x00)), readl(WDT_REG(0x04)), readl(WDT_REG(0x08)));
	if (!mmc)
		return CMD_RET_FAILURE;
	emmc_print(mmc, mmc_init(mmc));

	return CMD_RET_SUCCESS;
}

static int do_r76s_optee(struct cmd_tbl *cmdtp, int flag, int argc,
			 char *const argv[])
{
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS && gd->dram[i].size; i++) {
		if (OPTEE_BASE >= gd->dram[i].start &&
		    OPTEE_BASE - gd->dram[i].start < gd->dram[i].size) {
			printf("r76s-lab: OP-TEE memory at %#x is in DRAM bank %d\n",
			       OPTEE_BASE, i);
			return CMD_RET_FAILURE;
		}
	}

	return CMD_RET_SUCCESS;
}

static int do_r76s_regs(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	emmc_dump(argc > 1 ? argv[1] : "now");

	return CMD_RET_SUCCESS;
}

static int do_r76s_reprobe(struct cmd_tbl *cmdtp, int flag, int argc,
			   char *const argv[])
{
	struct mmc *mmc = find_mmc_device(0);
	int ret;

	if (!mmc)
		return CMD_RET_FAILURE;
	ret = device_remove(mmc->dev, DM_REMOVE_NORMAL);
	if (!ret)
		ret = device_probe(mmc->dev);
	printf("r76s-lab: eMMC controller probed again: %d\n", ret);
	if (ret)
		return CMD_RET_FAILURE;
	emmc_dump("after probing again");

	mmc->has_init = 0;
	ret = mmc_init(mmc);
	emmc_print(mmc, ret);

	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_r76s_readtest(struct cmd_tbl *cmdtp, int flag, int argc,
			    char *const argv[])
{
	ulong lba, blocks, loops, i, n, t, bad = 0;
	struct blk_desc *desc;
	u32 crc, first = 0;
	void *buf;

	if (argc != 5)
		return CMD_RET_USAGE;
	lba = hextoul(argv[1], NULL);
	blocks = hextoul(argv[2], NULL);
	loops = hextoul(argv[3], NULL);
	desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, 0);
	if (!desc)
		return CMD_RET_FAILURE;

	buf = map_sysmem(hextoul(argv[4], NULL), blocks * desc->blksz);
	for (i = 0; i < loops; i++) {
		t = get_timer(0);
		n = blk_dread(desc, lba, blocks, buf);
		t = get_timer(t);
		crc = crc32(0, buf, blocks * desc->blksz);
		if (!i)
			first = crc;
		if (n != blocks || crc != first)
			bad++;
		printf("r76s-lab: readtest %lu: %lu of %lu blocks at %#lx, crc32 %08x, %lu ms\n",
		       i, n, blocks, lba, crc, t);
	}
	unmap_sysmem(buf);
	printf("r76s-lab: readtest: %lu of %lu reads short or different\n",
	       bad, loops);

	return bad ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD_WITH_SUBCMDS(r76s, "NanoPi R76S lab diagnostics",
	"info - DRAM banks, ramlog, eMMC and watchdog state\n"
	"r76s optee - fail if the OP-TEE memory is in a DRAM bank\n"
	"r76s regs [label] - dump the eMMC controller and CRU registers\n"
	"r76s reprobe - probe the eMMC controller again, then initialize the card\n"
	"r76s readtest <lba> <blocks> <loops> <addr> - read eMMC blocks repeatedly (hex arguments)",
	U_BOOT_SUBCMD_MKENT(info, 1, 1, do_r76s_info),
	U_BOOT_SUBCMD_MKENT(optee, 1, 1, do_r76s_optee),
	U_BOOT_SUBCMD_MKENT(regs, 2, 1, do_r76s_regs),
	U_BOOT_SUBCMD_MKENT(reprobe, 1, 1, do_r76s_reprobe),
	U_BOOT_SUBCMD_MKENT(readtest, 5, 1, do_r76s_readtest));
