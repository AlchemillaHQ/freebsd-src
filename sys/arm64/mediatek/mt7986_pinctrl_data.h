/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#ifndef _MT7986_PINCTRL_DATA_H_
#define	_MT7986_PINCTRL_DATA_H_

#define	MT7986_NPINS	101
#define	MT7986_NBANKS	7

struct mt7986_bank {
	const char	*name;
	uint16_t	ies;
	uint16_t	smt;
	uint16_t	pu;
	uint16_t	pd;
	uint16_t	pupd;
	uint16_t	r0;
	uint16_t	r1;
};

static const struct mt7986_bank mt7986_banks[MT7986_NBANKS] = {
	{ "gpio",     0,    0,    0,    0,    0,    0,    0 },
	{ "iocfg_rt", 0x30, 0xc0, 0,    0,    0x40, 0x50, 0x60 },
	{ "iocfg_rb", 0x40, 0xf0, 0,    0,    0x60, 0x70, 0x80 },
	{ "iocfg_lt", 0x20, 0x90, 0,    0,    0x30, 0x40, 0x50 },
	{ "iocfg_lb", 0x20, 0x90, 0,    0,    0x40, 0x50, 0x60 },
	{ "iocfg_tr", 0x30, 0x80, 0x50, 0x40, 0,    0,    0 },
	{ "iocfg_tl", 0x30, 0x70, 0x50, 0x40, 0,    0,    0 },
};

struct mt7986_pad {
	const char	*name;
	uint8_t		bank;
	uint8_t		input_bit;
	uint8_t		pull_bit;
	uint8_t		drive_reg;
	uint8_t		drive_bit;
	uint8_t		drive_width;
};

/*
 * IES and SMT share a bit index. Drive width zero marks an unverified field.
 * The reference drive fields for pins 89 and 100 overlap at IOCFG_TL + 0x10.
 */
static const struct mt7986_pad mt7986_pads[MT7986_NPINS] = {
	[0] = { "SYS_WATCHDOG", 2, 17, 17, 0x10, 21, 3 },
	[1] = { "WF2G_LED", 3, 10, 10, 0x10, 0, 3 },
	[2] = { "WF5G_LED", 3, 11, 11, 0x10, 3, 3 },
	[3] = { "I2C_SCL", 4, 0, 0, 0x00, 0, 1 },
	[4] = { "I2C_SDA", 4, 1, 1, 0x00, 1, 1 },
	[5] = { "GPIO_0", 2, 0, 0, 0x00, 0, 3 },
	[6] = { "GPIO_1", 2, 1, 1, 0x00, 21, 3 },
	[7] = { "GPIO_2", 3, 0, 0, 0x00, 0, 3 },
	[8] = { "GPIO_3", 3, 1, 1, 0x00, 3, 3 },
	[9] = { "GPIO_4", 3, 2, 2, 0x00, 6, 3 },
	[10] = { "GPIO_5", 3, 3, 3, 0x00, 9, 3 },
	[11] = { "GPIO_6", 2, 8, 8, 0x00, 24, 3 },
	[12] = { "GPIO_7", 2, 9, 9, 0x00, 27, 3 },
	[13] = { "GPIO_8", 2, 10, 10, 0x10, 0, 3 },
	[14] = { "GPIO_9", 2, 11, 11, 0x10, 3, 3 },
	[15] = { "GPIO_10", 2, 2, 2, 0x00, 3, 3 },
	[16] = { "GPIO_11", 2, 3, 3, 0x00, 6, 3 },
	[17] = { "GPIO_12", 2, 4, 4, 0x00, 9, 3 },
	[18] = { "GPIO_13", 2, 5, 5, 0x00, 12, 3 },
	[19] = { "GPIO_14", 2, 6, 6, 0x00, 15, 3 },
	[20] = { "GPIO_15", 2, 7, 7, 0x00, 18, 3 },
	[21] = { "PWM0", 1, 12, 12, 0x10, 6, 3 },
	[22] = { "PWM1", 1, 13, 13, 0x10, 9, 3 },
	[23] = { "SPI0_CLK", 1, 14, 14, 0x10, 12, 3 },
	[24] = { "SPI0_MOSI", 1, 18, 18, 0x10, 24, 3 },
	[25] = { "SPI0_MISO", 1, 17, 17, 0x10, 21, 3 },
	[26] = { "SPI0_CS", 1, 15, 15, 0x10, 15, 3 },
	[27] = { "SPI0_HOLD", 1, 16, 16, 0x10, 18, 3 },
	[28] = { "SPI0_WP", 1, 19, 19, 0x10, 27, 3 },
	[29] = { "SPI1_CLK", 1, 20, 20, 0x20, 0, 3 },
	[30] = { "SPI1_MOSI", 1, 23, 23, 0x20, 9, 3 },
	[31] = { "SPI1_MISO", 1, 22, 22, 0x20, 6, 3 },
	[32] = { "SPI1_CS", 1, 21, 21, 0x20, 3, 3 },
	[33] = { "SPI2_CLK", 3, 4, 4, 0x00, 12, 3 },
	[34] = { "SPI2_MOSI", 3, 8, 8, 0x00, 24, 3 },
	[35] = { "SPI2_MISO", 3, 7, 7, 0x00, 21, 3 },
	[36] = { "SPI2_CS", 3, 5, 5, 0x00, 15, 3 },
	[37] = { "SPI2_HOLD", 3, 6, 6, 0x00, 18, 3 },
	[38] = { "SPI2_WP", 3, 9, 9, 0x00, 27, 3 },
	[39] = { "UART0_RXD", 2, 18, 18, 0x10, 27, 3 },
	[40] = { "UART0_TXD", 2, 19, 19, 0x20, 0, 3 },
	[41] = { "PCIE_PERESET_N", 2, 12, 12, 0x10, 6, 3 },
	[42] = { "UART1_RXD", 2, 22, 23, 0x20, 9, 3 },
	[43] = { "UART1_TXD", 2, 23, 24, 0x20, 12, 3 },
	[44] = { "UART1_CTS", 2, 20, 21, 0x20, 3, 3 },
	[45] = { "UART1_RTS", 2, 21, 22, 0x20, 6, 3 },
	[46] = { "UART2_RXD", 2, 26, 27, 0x20, 21, 3 },
	[47] = { "UART2_TXD", 2, 27, 28, 0x20, 24, 3 },
	[48] = { "UART2_CTS", 2, 24, 25, 0x20, 15, 3 },
	[49] = { "UART2_RTS", 2, 25, 26, 0x20, 18, 3 },
	[50] = { "EMMC_DATA_0", 1, 2, 2, 0x00, 6, 3 },
	[51] = { "EMMC_DATA_1", 1, 3, 3, 0x00, 9, 3 },
	[52] = { "EMMC_DATA_2", 1, 4, 4, 0x00, 12, 3 },
	[53] = { "EMMC_DATA_3", 1, 5, 5, 0x00, 15, 3 },
	[54] = { "EMMC_DATA_4", 1, 6, 6, 0x00, 18, 3 },
	[55] = { "EMMC_DATA_5", 1, 7, 7, 0x00, 21, 3 },
	[56] = { "EMMC_DATA_6", 1, 8, 8, 0x00, 24, 3 },
	[57] = { "EMMC_DATA_7", 1, 9, 9, 0x00, 27, 3 },
	[58] = { "EMMC_CMD", 1, 1, 1, 0x00, 3, 3 },
	[59] = { "EMMC_CK", 1, 0, 0, 0x00, 0, 3 },
	[60] = { "EMMC_DSL", 1, 10, 10, 0x10, 0, 3 },
	[61] = { "EMMC_RSTB", 1, 11, 11, 0x10, 3, 3 },
	[62] = { "PCM_DTX", 2, 15, 15, 0x10, 15, 3 },
	[63] = { "PCM_DRX", 2, 14, 14, 0x10, 12, 3 },
	[64] = { "PCM_CLK", 2, 13, 13, 0x10, 9, 3 },
	[65] = { "PCM_FS", 2, 16, 16, 0x10, 18, 3 },
	[66] = { "MT7531_INT", 4, 2, 2, 0x00, 2, 3 },
	[67] = { "SMI_MDC", 4, 3, 3, 0x00, 5, 3 },
	[68] = { "SMI_MDIO", 4, 4, 4, 0x00, 8, 3 },
	[69] = { "WF0_DIG_RESETB", 5, 1, 1, 0x00, 3, 3 },
	[70] = { "WF0_CBA_RESETB", 5, 0, 0, 0x00, 0, 3 },
	[71] = { "WF0_XO_REQ", 5, 16, 16, 0x10, 18, 3 },
	[72] = { "WF0_TOP_CLK", 5, 14, 14, 0x10, 12, 3 },
	[73] = { "WF0_TOP_DATA", 5, 15, 15, 0x10, 15, 3 },
	[74] = { "WF0_HB1", 5, 4, 4, 0x00, 15, 3 },
	[75] = { "WF0_HB2", 5, 6, 6, 0x00, 18, 3 },
	[76] = { "WF0_HB3", 5, 7, 7, 0x00, 21, 3 },
	[77] = { "WF0_HB4", 5, 8, 8, 0x00, 24, 3 },
	[78] = { "WF0_HB0", 5, 2, 2, 0x00, 6, 3 },
	[79] = { "WF0_HB0_B", 5, 3, 3, 0x00, 9, 3 },
	[80] = { "WF0_HB5", 5, 9, 9, 0x00, 27, 3 },
	[81] = { "WF0_HB6", 5, 10, 10, 0x10, 0, 3 },
	[82] = { "WF0_HB7", 5, 11, 11, 0x10, 3, 3 },
	[83] = { "WF0_HB8", 5, 12, 12, 0x10, 6, 3 },
	[84] = { "WF0_HB9", 5, 13, 13, 0x10, 9, 3 },
	[85] = { "WF0_HB10", 5, 5, 5, 0x00, 12, 3 },
	[86] = { "WF1_DIG_RESETB", 6, 1, 1, 0x00, 3, 3 },
	[87] = { "WF1_CBA_RESETB", 6, 0, 0, 0x00, 0, 3 },
	[88] = { "WF1_XO_REQ", 6, 14, 14, 0x10, 12, 3 },
	[89] = { "WF1_TOP_CLK", 6, 12, 12, 0x10, 6, 0 },
	[90] = { "WF1_TOP_DATA", 6, 13, 13, 0x10, 9, 3 },
	[91] = { "WF1_HB1", 6, 4, 4, 0x00, 12, 3 },
	[92] = { "WF1_HB2", 6, 5, 5, 0x00, 15, 3 },
	[93] = { "WF1_HB3", 6, 6, 6, 0x00, 18, 3 },
	[94] = { "WF1_HB4", 6, 7, 7, 0x00, 21, 3 },
	[95] = { "WF1_HB0", 6, 2, 2, 0x00, 6, 3 },
	[96] = { "WF1_HB0_B", 6, 3, 3, 0x00, 9, 3 },
	[97] = { "WF1_HB5", 6, 8, 8, 0x00, 24, 3 },
	[98] = { "WF1_HB6", 6, 9, 9, 0x00, 27, 3 },
	[99] = { "WF1_HB7", 6, 10, 10, 0x10, 2, 3 },
	[100] = { "WF1_HB8", 6, 11, 11, 0x10, 5, 0 },
};

struct mt7986_group {
	const char	*name;
	const char	*function;
	uint8_t		first;
	uint8_t		last;
	uint8_t		mode;
};

/* Each binding group uses one selector across a contiguous pin range. */
static const struct mt7986_group mt7986_groups[] = {
	{ "pcm", "audio", 62, 65, 1 },
	{ "i2s", "audio", 62, 65, 1 },
	{ "emmc_45", "emmc", 22, 32, 2 },
	{ "emmc_51", "emmc", 50, 61, 1 },
	{ "switch_int", "eth", 66, 66, 1 },
	{ "mdc_mdio", "eth", 67, 68, 1 },
	{ "snfi", "flash", 23, 28, 1 },
	{ "i2c", "i2c", 3, 4, 1 },
	{ "wifi_led", "led", 1, 2, 1 },
	{ "pcie_clk", "pcie", 9, 9, 1 },
	{ "pcie_wake", "pcie", 10, 10, 1 },
	{ "pcie_pereset", "pcie", 41, 41, 1 },
	{ "pwm0", "pwm", 21, 21, 1 },
	{ "pwm1_0", "pwm", 22, 22, 1 },
	{ "pwm1_1", "pwm", 20, 20, 2 },
	{ "spi0", "spi", 33, 36, 1 },
	{ "spi0_wp_hold", "spi", 37, 38, 1 },
	{ "spi1_0", "spi", 11, 14, 3 },
	{ "spi1_1", "spi", 23, 26, 3 },
	{ "spi1_2", "spi", 29, 32, 1 },
	{ "spi1_3", "spi", 33, 36, 4 },
	{ "uart0", "uart", 39, 40, 1 },
	{ "uart1", "uart", 42, 45, 1 },
	{ "uart1_0", "uart", 7, 10, 3 },
	{ "uart1_1", "uart", 23, 26, 4 },
	{ "uart1_rx_tx", "uart", 42, 43, 1 },
	{ "uart1_cts_rts", "uart", 44, 45, 1 },
	{ "uart1_2_rx_tx", "uart", 29, 30, 3 },
	{ "uart1_2_cts_rts", "uart", 31, 32, 3 },
	{ "uart1_3_rx_tx", "uart", 35, 36, 2 },
	{ "uart1_3_cts_rts", "uart", 37, 38, 2 },
	{ "uart2", "uart", 46, 49, 1 },
	{ "uart2_0_rx_tx", "uart", 29, 30, 4 },
	{ "uart2_0_cts_rts", "uart", 31, 32, 4 },
	{ "uart2_1", "uart", 33, 36, 3 },
	{ "watchdog", "watchdog", 0, 0, 1 },
	{ "wf_2g", "wifi", 74, 83, 1 },
	{ "wf_5g", "wifi", 91, 100, 1 },
	{ "wf_dbdc", "wifi", 74, 85, 2 },
};

#endif /* _MT7986_PINCTRL_DATA_H_ */
