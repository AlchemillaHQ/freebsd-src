/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 *
 * The address table and VLAN table definitions follow mtkswitch(4)'s
 * MT7530 definitions, Copyright (c) 2016 Stanislav Galabov.
 */

#ifndef _DEV_ETHERSWITCH_MT7531REG_H_
#define	_DEV_ETHERSWITCH_MT7531REG_H_

#define	MT7531_NPORTS		7
#define	MT7531_NPHYS		5	/* ports 0-4 have internal PHYs */
#define	MT7531_CPU_PORT		6
#define	MT7531_NVLANGROUPS	16
#define	MT7531_ALL_PORTS	((1u << MT7531_NPORTS) - 1)

/* Global control. */
#define	MT7531_CFC		0x0004
#define	 CFC_CPU_PMAP_MASK	0xff
#define	MT7531_MFC		0x0010
#define	 MFC_BC_FFP(x)		(((x) & 0xff) << 24)
#define	 MFC_UNM_FFP(x)		(((x) & 0xff) << 16)
#define	 MFC_UNU_FFP(x)		(((x) & 0xff) << 8)
#define	 MFC_FFP_MASK		0xffffff00u

/* Address table. */
#define	MT7531_ATC		0x0080
#define	 ATC_BUSY		(1u << 15)
#define	 ATC_MAT_NON_STATIC	(4u << 8)
#define	 ATC_CMD_CLEAN		(2u << 0)

/* VLAN table. */
#define	MT7531_VTCR		0x0090
#define	 VTCR_BUSY		(1u << 31)
#define	 VTCR_IDX_INVALID	(1u << 16)
#define	 VTCR_FUNC_READ		(0u << 12)
#define	 VTCR_FUNC_WRITE	(1u << 12)
#define	 VTCR_FUNC_INVALIDATE	(2u << 12)
#define	 VTCR_VID_MASK		0xfff
#define	MT7531_VAWD1		0x0094
#define	 VAWD1_IVL_MAC		(1u << 30)
#define	 VAWD1_VTAG_EN		(1u << 28)
#define	 VAWD1_MEMBER_SHIFT	16
#define	 VAWD1_MEMBER_MASK	0xff
#define	 VAWD1_VALID		(1u << 0)
#define	MT7531_VAWD2		0x0098
#define	 VAWD2_TAGGED(p)	(2u << ((p) * 2))
#define	 VAWD2_MASK(p)		(3u << ((p) * 2))

/* Per port. */
#define	MT7531_PCR(p)		(0x2004 + (p) * 0x100)
#define	 PCR_MATRIX(x)		(((x) & 0xff) << 16)
#define	 PCR_MATRIX_MASK	PCR_MATRIX(0xff)
#define	 PCR_PORT_VLAN_MASK	3u
#define	 PCR_PORT_VLAN_SECURITY	3u		/* forward by VLAN table */
#define	MT7531_PVC(p)		(0x2010 + (p) * 0x100)
#define	 PVC_EG_TAG_MASK	(7u << 8)
#define	 PVC_EG_TAG_TABLE	(0u << 8)	/* tag as the VLAN table says */
#define	 PVC_EG_TAG_CONSISTENT	(1u << 8)	/* keep the ingress tag state */
#define	 PVC_VLAN_ATTR_MASK	(3u << 6)	/* 0: user port */
#define	 PVC_SPEC_TAG		(1u << 5)
#define	MT7531_PPBV1(p)		(0x2014 + (p) * 0x100)
#define	 PPBV_PVID_MASK		0xfff
#define	MT7531_PMCR(p)		(0x3000 + (p) * 0x100)
#define	 PMCR_FORCE_LNK		(1u << 31)	/* force mode, per field */
#define	 PMCR_FORCE_SPD		(1u << 30)
#define	 PMCR_FORCE_DPX		(1u << 29)
#define	 PMCR_FORCE_RX_FC	(1u << 28)
#define	 PMCR_FORCE_TX_FC	(1u << 27)
#define	 PMCR_IPG_SHORT		(1u << 18)
#define	 PMCR_MAC_MODE		(1u << 16)
#define	 PMCR_TX_EN		(1u << 14)
#define	 PMCR_RX_EN		(1u << 13)
#define	 PMCR_BKOFF_EN		(1u << 9)
#define	 PMCR_BACKPR_EN		(1u << 8)
#define	 PMCR_RX_FC		(1u << 5)
#define	 PMCR_TX_FC		(1u << 4)
#define	 PMCR_SPEED_1000	(2u << 2)
#define	 PMCR_DPX		(1u << 1)
#define	 PMCR_LINK		(1u << 0)
#define	 PMCR_LINK_DOWN		PMCR_FORCE_LNK	/* forced, link bit clear */
#define	MT7531_PMSR(p)		(0x3008 + (p) * 0x100)
#define	 PMSR_LINK		(1u << 0)
#define	 PMSR_DPX		(1u << 1)
#define	 PMSR_SPEED(x)		(((x) >> 2) & 3)
#define	 PMSR_TX_FC		(1u << 4)
#define	 PMSR_RX_FC		(1u << 5)

/* SGMII PCS of ports 5 and 6, the same block as the SoC SGMIISYS. */
#define	MT7531_PCS_BASE(p)	(0x5000 + ((p) - 5) * 0x1000)
#define	 PCS_CONTROL_1		0x000
#define	  PCS_BMCR_ANENABLE	(1u << 12)
#define	  PCS_BMSR_LINK		(1u << 18)
#define	 PCS_LINK_TIMER		0x018
#define	 PCS_SGMII_MODE		0x020
#define	  SGMII_IF_MODE_SGMII	(1u << 0)
#define	  SGMII_SPEED_DUPLEX_AN	(1u << 1)
#define	  SGMII_REMOTE_FAULT_DIS (1u << 8)
#define	 PCS_RESERVED_0		0x034
#define	  PCS_SW_RESET		(1u << 0)
#define	 PCS_QPHY_PWR_STATE	0x0e8
#define	  PCS_PHYA_PWD		(1u << 4)
#define	 PCS_ANA_RGC3		0x128
#define	  RGC3_SPEED_MASK	(3u << 2)
#define	  RGC3_SPEED_3125M	(1u << 2)

/* System. */
#define	MT7531_SYS_CTRL		0x7000
#define	 SYS_CTRL_REG_RST	(1u << 0)
#define	 SYS_CTRL_SW_RST	(1u << 1)
#define	MT7531_PHY_IAC		0x701c
#define	 IAC_ACCESS		(1u << 31)
#define	 IAC_REG_SHIFT		25	/* register, or C45 device */
#define	 IAC_ADDR_SHIFT		20
#define	 IAC_CMD_C45_ADDR	(0u << 18)
#define	 IAC_CMD_WRITE		(1u << 18)
#define	 IAC_CMD_C22_READ	(2u << 18)
#define	 IAC_CMD_C45_READ	(3u << 18)
#define	 IAC_START_C22		(1u << 16)
#define	 IAC_START_C45		(0u << 16)
#define	 IAC_DATA_MASK		0xffff
#define	MT7531_TOP_SIG_SR	0x780c
#define	 TOP_SIG_DUAL_SGMII	(1u << 1)
#define	MT7531_CREV		0x781c
#define	 CREV_ID(x)		((x) >> 16)
#define	 CREV_REV(x)		((x) & 0xf)
#define	 MT7531_ID		0x7531
#define	MT7531_PLLGP_EN		0x7820
#define	 PLLGP_SW_PLLGP		(1u << 0)
#define	 PLLGP_SW_CLKSW		(1u << 1)
#define	 PLLGP_EN_COREPLL	(1u << 2)
#define	MT7531_PLLGP_CR0	0x78a8
#define	 PLLGP_CR0_PCW_CHG	(1u << 0)
#define	 PLLGP_CR0_SDM_PCW_SHIFT 1
#define	 PLLGP_CR0_SDM_PCW_MASK	(0x1fffffu << 1)
#define	 PLLGP_CR0_COREPLL_EN	(1u << 22)
#define	 PLLGP_CR0_POSDIV_SHIFT	23
#define	 PLLGP_CR0_POSDIV_MASK	(7u << 23)
#define	MT7531_ANA_PLLGP_CR2	0x78b0
#define	MT7531_ANA_PLLGP_CR5	0x78bc
#define	MT7531_SMT0_IOLB	0x7f04
#define	 SMT0_IOLB_MDC		(1u << 5)

/* Internal PHY registers. */
#define	MT7531_PHY_PAGE		0x1f
#define	MT7531_PHY_EXT_REG_14	0x14	/* page 1 */
#define	 PHY_EN_DOWNSHIFT	(1u << 4)
#define	MT7531_PHY_EXT_REG_17	0x17	/* page 1 */
#define	 PHY_LINKDOWN_POWER_SAVING (1u << 4)
#define	MT7531_PHY_DEV1E	0x1e
#define	 PHY_DEV1E_REG_0C6	0x0c6
#define	  PHY_POWER_SAVING_MASK	(3u << 8)
#define	MT7531_PHY_DEV1F	0x1f
#define	 CORE_PLL_GROUP4	0x403
#define	  BYPASS_MODE		(1u << 4)
#define	  POWER_ON_OFF		(1u << 5)

#endif /* _DEV_ETHERSWITCH_MT7531REG_H_ */
