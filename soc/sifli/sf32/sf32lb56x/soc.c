/*
 * Copyright (c) 2025 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/cache.h>
#include <zephyr/sys/util.h>

#define BOOTROM_BKP_REG             DT_REG_ADDR(DT_INST(0, sifli_sf32lb_rtc_backup))
#define BOOTROM_FLASH_OFF_DELAY_MSK GENMASK(11U, 4U)
#define BOOTROM_FLASH_ON_DELAY_MSK  GENMASK(23U, 12U)

void soc_early_init_hook(void)
{
	/* Switch LPSYS clock from DLL1 (240MHz) to HXT48 (48MHz).
	 * Matches RT-Thread bsp_init.c: HAL_RCC_LCPU_ClockSelect + SetDiv.
	 * Without this, USART4 runs at 240MHz/48 = 5Mbaud instead of 1Mbaud. */

	/* CSR: SEL_SYS[1:0] = 1 (HXT48), SEL_PERI[4] = 1 (HXT48) */
	hwp_lpsys_rcc->CSR = (hwp_lpsys_rcc->CSR & ~0x13U) | 0x11U;

	/* CFGR: HDIV1=1 (no divide), PDIV1=1 (÷2), PDIV2=3 (÷8) */
	hwp_lpsys_rcc->CFGR = (hwp_lpsys_rcc->CFGR & ~0x7F3FU) |
			       (1U << 0) | (1U << 8) | (3U << 12);

	/* Enable PINMUX2 and GPIO2 (LPSYS domain, PB pins) clocks early.
	 * LPSYS RCC is not yet managed by Zephyr clock control. */
	hwp_lpsys_rcc->ENR1 |= LPSYS_RCC_ENR1_PINMUX2_Msk;
	hwp_lpsys_rcc->ENR2 |= LPSYS_RCC_ENR2_GPIO2_Msk;

#if CONFIG_SF32LB56X_BOOTROM_FLASH_ON_DELAY_MS > 0 ||                                              \
	CONFIG_SF32LB56X_BOOTROM_FLASH_OFF_DELAY_MS > 0
	uint32_t val;

	val = sys_read32(BOOTROM_BKP_REG);
	val &= ~(BOOTROM_FLASH_OFF_DELAY_MSK | BOOTROM_FLASH_ON_DELAY_MSK);
	val |= FIELD_PREP(BOOTROM_FLASH_OFF_DELAY_MSK,
			  CONFIG_SF32LB56X_BOOTROM_FLASH_OFF_DELAY_MS) |
	       FIELD_PREP(BOOTROM_FLASH_ON_DELAY_MSK, CONFIG_SF32LB56X_BOOTROM_FLASH_ON_DELAY_MS);
	sys_write32(val, BOOTROM_BKP_REG);
#endif
}
