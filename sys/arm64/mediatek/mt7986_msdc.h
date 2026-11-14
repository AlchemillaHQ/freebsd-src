/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#ifndef _MT7986_MSDC_H_
#define _MT7986_MSDC_H_

#define MSDC_CFG			0x000
#define  CFG_MODE		(1u << 0)
#define  CFG_CKPDN		(1u << 1)
#define  CFG_RST			(1u << 2)
#define  CFG_PIO			(1u << 3)
#define  CFG_CKSTB		(1u << 7)
#define  CFG_DIV_MASK		(0xfffu << 8)
#define  CFG_MODE_MASK		(3u << 20)
#define  CFG_MODE_DDR		(2u << 20)
#define  CFG_MODE_HS400		(3u << 20)
#define  CFG_HS400		(1u << 22)
#define MSDC_IOCON		0x004
#define  IOCON_RSPL		(1u << 1)
#define  IOCON_DSPL		(1u << 2)
#define  IOCON_W_DSPL		(1u << 8)
#define  IOCON_EDGES		(IOCON_RSPL | IOCON_DSPL | IOCON_W_DSPL)
#define MSDC_PS			0x008
#define  PS_CDEN			(1u << 0)
#define  PS_DAT0			(1u << 16)
#define MSDC_INT			0x00c
#define MSDC_INTEN		0x010
#define  INT_CMDRDY		(1u << 8)
#define  INT_CMDTMO		(1u << 9)
#define  INT_RSPCRC		(1u << 10)
#define  INT_XFER		(1u << 12)
#define  INT_DMA_DONE		(1u << 13)
#define  INT_DATTMO		(1u << 14)
#define  INT_DATCRC		(1u << 15)
#define  INT_BD_CSUM		(1u << 17)
#define  INT_GPD_CSUM		(1u << 18)
#define  INT_DMA_PROTECT		(1u << 19)
#define  INT_AXI_ERR		(1u << 23)
#define  INT_CMD_MASK		(INT_CMDRDY | INT_CMDTMO | INT_RSPCRC)
#define  INT_DATA_ERR		(INT_DATTMO | INT_DATCRC | INT_BD_CSUM | \
				 INT_GPD_CSUM | INT_DMA_PROTECT | INT_AXI_ERR)
#define  INT_DATA_MASK		(INT_XFER | INT_DMA_DONE | INT_DATA_ERR)
#define MSDC_FIFOCS		0x014
#define  FIFO_CLEAR		(1u << 31)
#define MSDC_TXDATA		0x018
#define SDC_CFG			0x030
#define  SDC_BUSWIDTH_MASK	(3u << 16)
#define  SDC_SDIO		(1u << 19)
#define  SDC_SDIOIDE		(1u << 20)
#define  SDC_DTOC_MASK		(0xffu << 24)
#define SDC_CMD			0x034
#define  CMD_STOP		(1u << 14)
#define  CMD_WRITE		(1u << 13)
#define SDC_ARG			0x038
#define SDC_STS			0x03c
#define  STS_BUSY		(1u << 0)
#define  STS_CMD_BUSY		(1u << 1)
#define SDC_RESP(n)		(0x040 + (n) * 4)
#define SDC_BLK_NUM		0x050
#define SDC_DATCRC_STS		0x060
#define EMMC_STS		0x078
#define DMA_HIGH		0x08c
#define DMA_SA			0x090
#define DMA_CA			0x094
#define DMA_CTRL		0x098
#define  DMA_START		(1u << 0)
#define  DMA_STOP		(1u << 1)
#define  DMA_DESCRIPTOR		(1u << 8)
#define  DMA_LAST		(1u << 10)
#define  DMA_BURST64		(6u << 12)
#define DMA_CFG			0x09c
#define  DMA_ACTIVE		(1u << 0)
#define  DMA_CHECKSUM		(1u << 1)
#define  DMA_CHECKSUM_12B		(1u << 16)
#define DMA_LENGTH		0x0a8
#define PATCH0			0x0b0
#define PATCH1			0x0b4
#define PATCH2			0x0b8
#define  PATCH2_DMA_36BIT		(1u << 1)
#define EMMC50_CFG0		0x208
#define EMMC50_CFG3		0x220
#define  CFG3_OUTS_WR_MASK	0x1fu
#define SDC_FIFO_CFG		0x228

#define TOP_CONTROL		0x00
#define  TOP_DAT_RXDLY_MASK	(0x1fu << 7)
#define  TOP_DAT_RXDLY_SEL	(1u << 13)
#define  TOP_RX_ENHANCE		(1u << 15)
#define TOP_CMD			0x04
#define  TOP_CMD_RXDLY_MASK	(0x1fu << 5)
#define  TOP_CMD_RXDLY_SEL	(1u << 11)
#define TOP_DS_TUNE		0x0c
#define  DS_DLY1_MASK		(0x1fu << 10)
#define  DS_DLY_SEL		(1u << 16)

#define MSDC_MAX_DATA		(512 * 1024)
#define MSDC_MAX_SEGS		129
#define MSDC_DMA_MAX_ADDRESS	((UINT64_C(1) << 36) - 1)
#define MSDC_DESC_HWO		(1u << 0)
#define MSDC_DESC_BDP		(1u << 1)
#define MSDC_DESC_EOL		(1u << 0)
#define MSDC_MAX_FREQ		200000000
#define MSDC_DEFAULT_DELAY_FREQ	52000000	/* Tuned only above it. */
#define MSDC_DELAYS		32
#define MSDC_TIMEOUT		(10 * SBT_1S)
#define MSDC_WRITE_TIMEOUT	(30 * SBT_1S)
#define MSDC_BUSTEST_TIMEOUT	SBT_1S
#define MSDC_TUNING_TIMEOUT	(100 * SBT_1MS)

#endif
