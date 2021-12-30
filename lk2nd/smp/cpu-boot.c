// SPDX-License-Identifier: GPL-2.0-only
#include <arch/defines.h>
#include <bits.h>
#include <debug.h>
#include <platform/timer.h>
#include <reg.h>
#include <scm.h>

#include "cpu-boot.h"

#define CPU_PWR_CTL			0x4
#define APC_PWR_GATE_CTL		0x14

#define CPU_PWR_CTL_CLAMP		BIT(0)
#define CPU_PWR_CTL_CORE_MEM_CLAMP	BIT(1)
#define CPU_PWR_CTL_L1_RST_DIS		BIT(2)
#define CPU_PWR_CTL_CORE_MEM_HS		BIT(3)
#define CPU_PWR_CTL_CORE_RST		BIT(4)
#define CPU_PWR_CTL_COREPOR_RST		BIT(5)
#define CPU_PWR_CTL_GATE_CLK		BIT(6)
#define CPU_PWR_CTL_CORE_PWRD_UP	BIT(7)

#define APC_PWR_GATE_CTL_GHDS_EN	BIT(0)
#define APC_PWR_GATE_CTL_GHDS_CNT(cnt)	((cnt) << 24)

#define QCOM_SCM_BOOT_SET_ADDR_MC	0x11
#define QCOM_SCM_BOOT_MC_FLAG_AARCH64	BIT(0)
#define QCOM_SCM_BOOT_MC_FLAG_COLDBOOT	BIT(1)
#define QCOM_SCM_BOOT_MC_FLAG_WARMBOOT	BIT(2)

#define A53PLL_MODE_REG			0x00
#define A53PLL_L_REG			0x04
#define A53PLL_M_REG			0x08
#define A53PLL_N_REG			0x0c
#define A53PLL_USER_REG			0x10
#define A53PLL_CONFIG_REG		0x14
#define A53PLL_STATUS_REG		0x1c
#define APCS_ALIAS0_CMD_RCGR		0xb111050
#define APCS_ALIAS0_CFG_OFF		0x4
#define APCS_ALIAS0_CORE_CBCR_OFF	0x8
#define L2_PWR_CTL_OVERRIDE		0xc
#define L2_PWR_CTL			0x14
#define L2_PWR_STATUS			0x18
#define L2_CORE_CBCR			0x58

static inline uint32_t read_mpidr(void)
{
	uint32_t res;
	__asm__ ("mrc p15, 0, %0, c0, c0, 5" : "=r" (res));
	return res & 0x00ffffff;
}

int qcom_set_boot_addr(uint32_t addr)
{
	scmcall_arg arg = {
		MAKE_SIP_SCM_CMD(SCM_SVC_BOOT, QCOM_SCM_BOOT_SET_ADDR_MC),
		MAKE_SCM_ARGS(6),
		addr,
		~0UL, ~0UL, ~0UL, ~0UL, /* All CPUs */
		QCOM_SCM_BOOT_MC_FLAG_AARCH64 | QCOM_SCM_BOOT_MC_FLAG_COLDBOOT,
	};
	return scm_call2(&arg, NULL);
}

void qcom_power_up_arm_cortex(uint32_t mpidr, uint32_t base)
{
	uint32_t pwr_ctl;

	if (mpidr == read_mpidr()) {
		dprintf(INFO, "Skipping boot of current CPU (%d)\n", mpidr);
		return;
	}

	pwr_ctl = CPU_PWR_CTL_CLAMP | CPU_PWR_CTL_CORE_MEM_CLAMP |
		  CPU_PWR_CTL_CORE_RST | CPU_PWR_CTL_COREPOR_RST;
	writel(pwr_ctl, base + CPU_PWR_CTL);
	dsb();

	writel(APC_PWR_GATE_CTL_GHDS_EN | APC_PWR_GATE_CTL_GHDS_CNT(16),
	       base + APC_PWR_GATE_CTL);
	dsb();
	udelay(2);

	pwr_ctl &= ~CPU_PWR_CTL_CORE_MEM_CLAMP;
	writel(pwr_ctl, base + CPU_PWR_CTL);
	dsb();

	pwr_ctl |= CPU_PWR_CTL_CORE_MEM_HS;
	writel(pwr_ctl, base + CPU_PWR_CTL);
	dsb();
	udelay(2);

	pwr_ctl &= ~CPU_PWR_CTL_CLAMP;
	writel(pwr_ctl, base + CPU_PWR_CTL);
	dsb();
	udelay(2);

	pwr_ctl &= ~(CPU_PWR_CTL_CORE_RST | CPU_PWR_CTL_COREPOR_RST);
	writel(pwr_ctl, base + CPU_PWR_CTL);
	dsb();

	pwr_ctl |= CPU_PWR_CTL_CORE_PWRD_UP;
	writel(pwr_ctl, base + CPU_PWR_CTL);
	dsb();

	/* Give CPU some time to boot */
	udelay(100);
}

void qcom_power_up_l2cache(uint32_t base)
{
	dprintf(INFO, "Powering l2cache at %x\n", base);

	if (readl(base + L2_PWR_STATUS) & 0x200) { // BIT(9)
		dprintf(INFO, "L2 cache at %x already powered-up\n", base);
		return;
	}

	writel(0x10d700, base + L2_PWR_CTL);
	dsb();
	writel(0x400000, base + L2_PWR_CTL_OVERRIDE);
	dsb();
	udelay(2);
	writel(0x101700, base + L2_PWR_CTL);
	dsb();
	writel(0x101703, base + L2_PWR_CTL);
	dsb();
	udelay(2);
	writel(0x1, base + L2_CORE_CBCR);
	dsb();
	writel(0x101603, base + L2_PWR_CTL);
	dsb();
	udelay(2);
	writel(0x0, base + L2_PWR_CTL_OVERRIDE);
	dsb();
	writel(0x100203, base + L2_PWR_CTL);
	dsb();
	udelay(54);
	writel(0x10100203, base + L2_PWR_CTL);
	dsb();
	writel(0x3, base + L2_CORE_CBCR);
	dsb();

	udelay(200);
}

void qcom_power_up_arm_cortex_pll(uint32_t base, uint32_t l, uint32_t m, uint32_t n, bool enable)
{
	dprintf(INFO, "Powering PLL (base=%d)\n", base);

	/* Disable PLL to be safe for programming */
	writel(0x0, base + A53PLL_MODE_REG);
	dsb();

	/* Configure L/M/N values with the first freq_tbl entry */
	writel(l, base + A53PLL_L_REG);
	dsb();
	writel(m, base + A53PLL_M_REG);
	dsb();
	writel(n, base + A53PLL_N_REG);
	dsb();

	/* Configure USER_CTL and CONFIG_CTL value */
	writel(0x0100000f, base + A53PLL_USER_REG);
	dsb();
	writel(0x4c015765, base + A53PLL_CONFIG_REG);
	dsb();

	if (enable) {
		writel(0x2, base + A53PLL_MODE_REG);
		dsb();
		udelay(2);
		writel(0x6, base + A53PLL_MODE_REG);
		dsb();
		udelay(50);
		writel(0x7, base + A53PLL_MODE_REG);
		dsb();
	}
}

void qcom_power_up_arm_cortex_pll_power_clocks(void)
{
	uint32_t reg;

	dprintf(INFO, "Powering PLL clocks\n");

	/* Source GPLL0 and 1/2 the rate of GPLL0 */
	writel(0x403, APCS_ALIAS0_CMD_RCGR + APCS_ALIAS0_CFG_OFF);
	dsb();
	reg = readl(APCS_ALIAS0_CMD_RCGR);
	reg |= 0x1;
	writel(reg, APCS_ALIAS0_CMD_RCGR);
	dsb();
	for (int count = 500; count > 0; count --) {
		if (!(readl(APCS_ALIAS0_CMD_RCGR) & 0x1))
			break;
		udelay(1);
	}
	/* Enable the branch */
	reg = readl(APCS_ALIAS0_CMD_RCGR + APCS_ALIAS0_CORE_CBCR_OFF);
	reg |= 0x1;
	writel(reg, APCS_ALIAS0_CMD_RCGR + APCS_ALIAS0_CORE_CBCR_OFF);
	dsb();
}
