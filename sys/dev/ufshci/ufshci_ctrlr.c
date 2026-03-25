/*-
 * Copyright (c) 2025, Samsung Electronics Co., Ltd.
 * Written by Jaeyoon Choi
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/conf.h>

#include "ufshci_private.h"
#include "ufshci_reg.h"

#define UFSHCI_QCOM_CORE_CLK_300MHZ 300
#define UFSHCI_QCOM_QMP_PHY_INIT_TIMEOUT_US 10000
#define UFSHCI_QCOM_QMP_PHY_INIT_POLL_US 200
#define UFSHCI_QCOM_QMP_PHY_V6_SERDES_OFF 0x0000
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF 0x0400
#define UFSHCI_QCOM_QMP_PHY_V6_TX_OFF 0x1000
#define UFSHCI_QCOM_QMP_PHY_V6_RX_OFF 0x1200
#define UFSHCI_QCOM_QMP_PHY_V6_TX2_OFF 0x1800
#define UFSHCI_QCOM_QMP_PHY_V6_RX2_OFF 0x1a00

#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_POWER_DOWN_CONTROL 0x104
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_SW_RESET 0x120
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_PHY_START 0x124
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_PLL_CNTL 0x12c
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_TX_HSGEAR_CAPABILITY 0x0b8
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_RX_HSGEAR_CAPABILITY 0x0bc
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_READY_STATUS 0x1a8
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_RX_SIGDET_CTRL2 0x1ac
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_TX_LARGE_AMP_DRV_LVL 0x1f0
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_TX_MID_TERM_CTRL1 0x1f4
#define UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_MULTI_LANE_CTRL1 0x1fc

#define UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP_EN 0x028
#define UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP1_MODE0 0x06c
#define UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP2_MODE0 0x070
#define UFSHCI_QCOM_QSERDES_V6_COM_CP_CTRL_MODE0 0x074
#define UFSHCI_QCOM_QSERDES_V6_COM_CP_CTRL_MODE1 0x078
#define UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP1_MODE1 0x07c
#define UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP2_MODE1 0x080
#define UFSHCI_QCOM_QSERDES_V6_COM_PLL_RCTRL_MODE0 0x084
#define UFSHCI_QCOM_QSERDES_V6_COM_PLL_RCTRL_MODE1 0x08c
#define UFSHCI_QCOM_QSERDES_V6_COM_PLL_CCTRL_MODE0 0x094
#define UFSHCI_QCOM_QSERDES_V6_COM_PLL_CCTRL_MODE1 0x09c
#define UFSHCI_QCOM_QSERDES_V6_COM_SYSCLK_EN_SEL 0x110
#define UFSHCI_QCOM_QSERDES_V6_COM_HSCLK_SEL_1 0x158
#define UFSHCI_QCOM_QSERDES_V6_COM_HSCLK_HS_SWITCH_SEL_1 0x15c
#define UFSHCI_QCOM_QSERDES_V6_COM_CMN_CONFIG_1 0x174
#define UFSHCI_QCOM_QSERDES_V6_COM_DEC_START_MODE0 0x17c
#define UFSHCI_QCOM_QSERDES_V6_COM_DEC_START_MODE1 0x180
#define UFSHCI_QCOM_QSERDES_V6_COM_VCO_TUNE_MAP 0x1c8
#define UFSHCI_QCOM_QSERDES_V6_COM_VCO_TUNE_INITVAL2 0x1d0
#define UFSHCI_QCOM_QSERDES_V6_COM_PLL_IVCO 0x1f4

#define UFSHCI_QCOM_QSERDES_UFS_V6_TX_RES_CODE_LANE_OFFSET_TX 0x03c
#define UFSHCI_QCOM_QSERDES_UFS_V6_TX_LANE_MODE_1 0x084
#define UFSHCI_QCOM_QSERDES_UFS_V6_TX_FR_DCC_CTRL 0x0ec

#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_UCDR_FO_GAIN_RATE2 0x0d4
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_VGA_CAL_MAN_VAL 0x178
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE_0_1_B0 0x208
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE_0_1_B1 0x20c
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE_0_1_B3 0x214
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE_0_1_B6 0x220
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE2_B3 0x234
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE2_B6 0x240
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE3_B3 0x25c
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE3_B4 0x260
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE3_B5 0x264
#define UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE3_B8 0x270

#define UFSHCI_QCOM_QMP_PHY_SW_RESET (1U << 0)
#define UFSHCI_QCOM_QMP_PHY_SW_PWRDN (1U << 0)
#define UFSHCI_QCOM_QMP_PHY_SERDES_START (1U << 0)
#define UFSHCI_QCOM_QMP_PHY_PCS_READY (1U << 0)
#define UFSHCI_QCOM_GCC_UFS_PHY_GDSCR 0x77004
#define UFSHCI_QCOM_GCC_UFS_PHY_AXI_CLK_CBCR 0x77018
#define UFSHCI_QCOM_GCC_UFS_PHY_AHB_CLK_CBCR 0x77024
#define UFSHCI_QCOM_GCC_UFS_PHY_TX_SYMBOL_0_CLK_CBCR 0x77028
#define UFSHCI_QCOM_GCC_UFS_PHY_RX_SYMBOL_0_CLK_CBCR 0x7702c
#define UFSHCI_QCOM_GCC_UFS_PHY_AXI_CLK_SRC_CMD_RCGR 0x77030
#define UFSHCI_QCOM_GCC_UFS_PHY_UNIPRO_CORE_CLK_CBCR 0x77068
#define UFSHCI_QCOM_GCC_UFS_PHY_ICE_CORE_CLK_CBCR 0x77074
#define UFSHCI_QCOM_GCC_UFS_PHY_ICE_CORE_CLK_SRC_CMD_RCGR 0x77080
#define UFSHCI_QCOM_GCC_UFS_PHY_UNIPRO_CORE_CLK_SRC_CMD_RCGR 0x77098
#define UFSHCI_QCOM_GCC_UFS_PHY_PHY_AUX_CLK_CBCR 0x770b0
#define UFSHCI_QCOM_GCC_UFS_PHY_RX_SYMBOL_1_CLK_CBCR 0x770cc
#define UFSHCI_QCOM_GCC_AGGRE_UFS_PHY_AXI_CLK_CBCR 0x770e4
#define UFSHCI_QCOM_TCSR_UFS_PHY_CLKREF_EN 0x15118
#define UFSHCI_QCOM_RCG_CFG_REG 0x4
#define UFSHCI_QCOM_RCG_CMD_UPDATE (1U << 0)
#define UFSHCI_QCOM_RCG_CFG_SRC_SEL_SHIFT 8
#define UFSHCI_QCOM_GDSCR_SW_COLLAPSE (1U << 0)
#define UFSHCI_QCOM_GDSCR_HW_CONTROL (1U << 1)
#define UFSHCI_QCOM_GDSCR_SW_OVERRIDE (1U << 2)
#define UFSHCI_QCOM_GDSCR_RETAIN_FF_ENABLE (1U << 11)
#define UFSHCI_QCOM_GDSCR_CLK_DIS_WAIT_SHIFT 12
#define UFSHCI_QCOM_GDSCR_EN_FEW_WAIT_SHIFT 16
#define UFSHCI_QCOM_GDSCR_EN_REST_WAIT_SHIFT 20
#define UFSHCI_QCOM_GDSCR_CFG_OFFSET 0x4
#define UFSHCI_QCOM_GDSCR_POWER_UP_COMPLETE (1U << 16)
#define UFSHCI_QCOM_GCC_PARENT_GPLL0_OUT_MAIN 1
#define UFSHCI_QCOM_GCC_PARENT_GPLL4_OUT_MAIN 5
#define UFSHCI_QCOM_RCG_HID_DIV(pre_div) (((pre_div) << 1) - 1)

struct ufshci_qcom_qmp_reg_val {
	bus_size_t reg;
	uint32_t val;
};

static const struct ufshci_qcom_qmp_reg_val ufshci_qcom_sm8550_ufsphy_serdes[] = {
	{ UFSHCI_QCOM_QSERDES_V6_COM_SYSCLK_EN_SEL, 0xd9 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_CMN_CONFIG_1, 0x16 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_HSCLK_SEL_1, 0x11 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_HSCLK_HS_SWITCH_SEL_1, 0x00 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP_EN, 0x01 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_VCO_TUNE_INITVAL2, 0x00 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_DEC_START_MODE0, 0x41 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_PLL_RCTRL_MODE0, 0x18 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_PLL_CCTRL_MODE0, 0x14 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP1_MODE0, 0x7f },
	{ UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP2_MODE0, 0x06 },
};

static const struct ufshci_qcom_qmp_reg_val
    ufshci_qcom_sm8550_ufsphy_hs_b_serdes[] = {
	{ UFSHCI_QCOM_QSERDES_V6_COM_VCO_TUNE_MAP, 0x44 },
};

static const struct ufshci_qcom_qmp_reg_val
    ufshci_qcom_sm8550_ufsphy_g4_serdes[] = {
	{ UFSHCI_QCOM_QSERDES_V6_COM_VCO_TUNE_MAP, 0x04 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_PLL_IVCO, 0x0f },
	{ UFSHCI_QCOM_QSERDES_V6_COM_CP_CTRL_MODE0, 0x0a },
	{ UFSHCI_QCOM_QSERDES_V6_COM_DEC_START_MODE1, 0x4c },
	{ UFSHCI_QCOM_QSERDES_V6_COM_CP_CTRL_MODE1, 0x0a },
	{ UFSHCI_QCOM_QSERDES_V6_COM_PLL_RCTRL_MODE1, 0x18 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_PLL_CCTRL_MODE1, 0x14 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP1_MODE1, 0x99 },
	{ UFSHCI_QCOM_QSERDES_V6_COM_LOCK_CMP2_MODE1, 0x07 },
};

static const struct ufshci_qcom_qmp_reg_val ufshci_qcom_sm8550_ufsphy_tx[] = {
	{ UFSHCI_QCOM_QSERDES_UFS_V6_TX_LANE_MODE_1, 0x05 },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_TX_RES_CODE_LANE_OFFSET_TX, 0x07 },
};

static const struct ufshci_qcom_qmp_reg_val ufshci_qcom_sm8550_ufsphy_g4_tx[] = {
	{ UFSHCI_QCOM_QSERDES_UFS_V6_TX_FR_DCC_CTRL, 0x4c },
};

static const struct ufshci_qcom_qmp_reg_val ufshci_qcom_sm8550_ufsphy_rx[] = {
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_UCDR_FO_GAIN_RATE2, 0x0c },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE_0_1_B0, 0xc2 },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE_0_1_B1, 0xc2 },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE_0_1_B3, 0x1a },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE_0_1_B6, 0x60 },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE2_B3, 0x9e },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE2_B6, 0x60 },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE3_B3, 0x9e },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE3_B4, 0x0e },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE3_B5, 0x36 },
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_MODE_RATE3_B8, 0x02 },
};

static const struct ufshci_qcom_qmp_reg_val ufshci_qcom_sm8550_ufsphy_g4_rx[] = {
	{ UFSHCI_QCOM_QSERDES_UFS_V6_RX_VGA_CAL_MAN_VAL, 0x0e },
};

static const struct ufshci_qcom_qmp_reg_val ufshci_qcom_sm8550_ufsphy_pcs[] = {
	{ UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_RX_SIGDET_CTRL2, 0x69 },
	{ UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_TX_LARGE_AMP_DRV_LVL, 0x0f },
	{ UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_TX_MID_TERM_CTRL1, 0x43 },
	{ UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_MULTI_LANE_CTRL1, 0x02 },
};

static const struct ufshci_qcom_qmp_reg_val ufshci_qcom_sm8550_ufsphy_g4_pcs[] = {
	{ UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_PLL_CNTL, 0x2b },
	{ UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_TX_HSGEAR_CAPABILITY, 0x04 },
	{ UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_RX_HSGEAR_CAPABILITY, 0x04 },
};

static void
ufshci_ctrlr_fail(struct ufshci_controller *ctrlr)
{
	ctrlr->is_failed = true;

	ufshci_req_queue_fail(ctrlr, &ctrlr->task_mgmt_req_queue);
	ufshci_req_queue_fail(ctrlr, &ctrlr->transfer_req_queue);
}

static bool
ufshci_ctrlr_qcom_has_gcc_mmio(struct ufshci_controller *ctrlr)
{

	return (ctrlr->qcom_gcc_is_direct_map);
}

static bool
ufshci_ctrlr_qcom_has_tcsr_mmio(struct ufshci_controller *ctrlr)
{

	return (ctrlr->qcom_tcsr_is_direct_map);
}

static uint32_t
ufshci_ctrlr_qcom_gcc_read_4(struct ufshci_controller *ctrlr, bus_size_t reg)
{

	KASSERT(ufshci_ctrlr_qcom_has_gcc_mmio(ctrlr),
	    ("QCOM GCC MMIO is not mapped"));
	return (bus_space_read_4(ctrlr->qcom_gcc_bus_tag,
	    ctrlr->qcom_gcc_bus_handle, reg));
}

static void
ufshci_ctrlr_qcom_gcc_write_4(struct ufshci_controller *ctrlr, bus_size_t reg,
    uint32_t val)
{

	KASSERT(ufshci_ctrlr_qcom_has_gcc_mmio(ctrlr),
	    ("QCOM GCC MMIO is not mapped"));
	bus_space_write_4(ctrlr->qcom_gcc_bus_tag, ctrlr->qcom_gcc_bus_handle,
	    reg, val);
	(void)bus_space_read_4(ctrlr->qcom_gcc_bus_tag,
	    ctrlr->qcom_gcc_bus_handle, reg);
}

static uint32_t
ufshci_ctrlr_qcom_tcsr_read_4(struct ufshci_controller *ctrlr, bus_size_t reg)
{

	KASSERT(ufshci_ctrlr_qcom_has_tcsr_mmio(ctrlr),
	    ("QCOM TCSR MMIO is not mapped"));
	return (bus_space_read_4(ctrlr->qcom_tcsr_bus_tag,
	    ctrlr->qcom_tcsr_bus_handle, reg));
}

static void
ufshci_ctrlr_qcom_tcsr_write_4(struct ufshci_controller *ctrlr, bus_size_t reg,
    uint32_t val)
{

	KASSERT(ufshci_ctrlr_qcom_has_tcsr_mmio(ctrlr),
	    ("QCOM TCSR MMIO is not mapped"));
	bus_space_write_4(ctrlr->qcom_tcsr_bus_tag, ctrlr->qcom_tcsr_bus_handle,
	    reg, val);
	(void)bus_space_read_4(ctrlr->qcom_tcsr_bus_tag,
	    ctrlr->qcom_tcsr_bus_handle, reg);
}

static int
ufshci_ctrlr_qcom_update_rcgr(struct ufshci_controller *ctrlr,
    bus_size_t cmd_rcgr)
{
	uint32_t cmd;
	int timeout_us;

	ufshci_ctrlr_qcom_gcc_write_4(ctrlr, cmd_rcgr,
	    UFSHCI_QCOM_RCG_CMD_UPDATE);
	for (timeout_us = 0; timeout_us < 500; timeout_us++) {
		cmd = ufshci_ctrlr_qcom_gcc_read_4(ctrlr, cmd_rcgr);
		if ((cmd & UFSHCI_QCOM_RCG_CMD_UPDATE) == 0)
			return (0);
		DELAY(1);
	}

	ufshci_printf(ctrlr, "QCOM GCC RCG update timed out at %#zx\n",
	    cmd_rcgr);
	return (ETIMEDOUT);
}

static int
ufshci_ctrlr_qcom_configure_rcgr(struct ufshci_controller *ctrlr,
    bus_size_t cmd_rcgr, uint32_t parent_cfg, uint32_t pre_div)
{
	uint32_t cfg;

	cfg = UFSHCI_QCOM_RCG_HID_DIV(pre_div);
	cfg |= parent_cfg << UFSHCI_QCOM_RCG_CFG_SRC_SEL_SHIFT;
	ufshci_ctrlr_qcom_gcc_write_4(ctrlr, cmd_rcgr + UFSHCI_QCOM_RCG_CFG_REG,
	    cfg);
	return (ufshci_ctrlr_qcom_update_rcgr(ctrlr, cmd_rcgr));
}

static void
ufshci_ctrlr_qcom_enable_branch(struct ufshci_controller *ctrlr,
    bus_size_t cbcr)
{
	uint32_t val;

	val = ufshci_ctrlr_qcom_gcc_read_4(ctrlr, cbcr);
	if ((val & 0x1) != 0)
		return;
	val |= 0x1;
	ufshci_ctrlr_qcom_gcc_write_4(ctrlr, cbcr, val);
}

static int
ufshci_ctrlr_qcom_enable_gdsc(struct ufshci_controller *ctrlr,
    bus_size_t gdscr)
{
	uint32_t val;
	int timeout_us;

	val = ufshci_ctrlr_qcom_gcc_read_4(ctrlr, gdscr);
	val &= ~(UFSHCI_QCOM_GDSCR_SW_COLLAPSE |
	    UFSHCI_QCOM_GDSCR_HW_CONTROL |
	    UFSHCI_QCOM_GDSCR_SW_OVERRIDE);
	val |= UFSHCI_QCOM_GDSCR_RETAIN_FF_ENABLE;
	val &= ~((0xf << UFSHCI_QCOM_GDSCR_CLK_DIS_WAIT_SHIFT) |
	    (0xf << UFSHCI_QCOM_GDSCR_EN_FEW_WAIT_SHIFT) |
	    (0xf << UFSHCI_QCOM_GDSCR_EN_REST_WAIT_SHIFT));
	val |= 0xf << UFSHCI_QCOM_GDSCR_CLK_DIS_WAIT_SHIFT;
	val |= 0x2 << UFSHCI_QCOM_GDSCR_EN_FEW_WAIT_SHIFT;
	val |= 0x2 << UFSHCI_QCOM_GDSCR_EN_REST_WAIT_SHIFT;
	ufshci_ctrlr_qcom_gcc_write_4(ctrlr, gdscr, val);

	for (timeout_us = 0; timeout_us < 2000; timeout_us++) {
		val = ufshci_ctrlr_qcom_gcc_read_4(ctrlr,
		    gdscr + UFSHCI_QCOM_GDSCR_CFG_OFFSET);
		if ((val & UFSHCI_QCOM_GDSCR_POWER_UP_COMPLETE) != 0)
			return (0);
		DELAY(1);
	}

	ufshci_printf(ctrlr, "QCOM GCC GDSC power-up timed out at %#zx\n",
	    gdscr);
	return (ETIMEDOUT);
}

static int
ufshci_ctrlr_qcom_prepare_acpi_bringup(struct ufshci_controller *ctrlr)
{
	int error;

	/*
	 * Execute the minimum X1E80100 ACPI PEP subset we can currently drive
	 * directly: PHY GDSC, GCC clock roots/branches, and the TCSR clkref.
	 */
	if (!ctrlr->qcom_acpi_bringup_enabled || ctrlr->qcom_acpi_pwrseq_done)
		return (0);
	if (!ctrlr->qcom_acpi_bringup_ready ||
	    !ufshci_ctrlr_qcom_has_gcc_mmio(ctrlr) ||
	    !ufshci_ctrlr_qcom_has_tcsr_mmio(ctrlr)) {
		ufshci_printf(ctrlr,
		    "QCOM ACPI bring-up backend unavailable, keeping initial UIC power mode\n");
		return (ENXIO);
	}

	error = ufshci_ctrlr_qcom_enable_gdsc(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_GDSCR);
	if (error != 0)
		return (error);

	error = ufshci_ctrlr_qcom_configure_rcgr(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_AXI_CLK_SRC_CMD_RCGR,
	    UFSHCI_QCOM_GCC_PARENT_GPLL0_OUT_MAIN, 2);
	if (error != 0)
		return (error);
	error = ufshci_ctrlr_qcom_configure_rcgr(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_UNIPRO_CORE_CLK_SRC_CMD_RCGR,
	    UFSHCI_QCOM_GCC_PARENT_GPLL0_OUT_MAIN, 2);
	if (error != 0)
		return (error);
	error = ufshci_ctrlr_qcom_configure_rcgr(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_ICE_CORE_CLK_SRC_CMD_RCGR,
	    UFSHCI_QCOM_GCC_PARENT_GPLL4_OUT_MAIN, 2);
	if (error != 0)
		return (error);

	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_AXI_CLK_CBCR);
	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_UNIPRO_CORE_CLK_CBCR);
	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_ICE_CORE_CLK_CBCR);
	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_AGGRE_UFS_PHY_AXI_CLK_CBCR);
	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_AHB_CLK_CBCR);
	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_PHY_AUX_CLK_CBCR);
	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_TX_SYMBOL_0_CLK_CBCR);
	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_RX_SYMBOL_0_CLK_CBCR);
	ufshci_ctrlr_qcom_enable_branch(ctrlr,
	    UFSHCI_QCOM_GCC_UFS_PHY_RX_SYMBOL_1_CLK_CBCR);

	ufshci_ctrlr_qcom_tcsr_write_4(ctrlr, UFSHCI_QCOM_TCSR_UFS_PHY_CLKREF_EN,
	    ufshci_ctrlr_qcom_tcsr_read_4(ctrlr,
	    UFSHCI_QCOM_TCSR_UFS_PHY_CLKREF_EN) | 0x1);

	ctrlr->qcom_acpi_pwrseq_done = true;
	ufshci_printf(ctrlr,
	    "QCOM ACPI direct PHY power sequence enabled via GCC/TCSR\n");
	return (0);
}

static uint32_t
ufshci_ctrlr_qcom_get_hw_major(struct ufshci_controller *ctrlr)
{
	uint32_t hw_ver;

	hw_ver = bus_space_read_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_HW_VERSION);

	return (UFSHCIV(UFSHCI_QCOM_HW_VERSION_REG_MAJOR, hw_ver));
}

static bool
ufshci_ctrlr_qcom_get_phy_mmio(struct ufshci_controller *ctrlr,
    bus_space_tag_t *tag, bus_space_handle_t *handle,
    bus_size_t *base_offset)
{
	int mmio_offset;

	if (ctrlr->qcom_acpi_bringup_enabled && !ctrlr->qcom_acpi_pwrseq_done &&
	    (ctrlr->qcom_phy_resource != NULL || ctrlr->qcom_phy_is_direct_map))
		return (false);

	if (ctrlr->qcom_phy_resource != NULL) {
		*tag = ctrlr->qcom_phy_bus_tag;
		*handle = ctrlr->qcom_phy_bus_handle;
		*base_offset = 0;
		return (true);
	}

	if (ctrlr->qcom_phy_is_direct_map) {
		*tag = ctrlr->qcom_phy_bus_tag;
		*handle = ctrlr->qcom_phy_bus_handle;
		*base_offset = 0;
		return (true);
	}

	mmio_offset = 0;
	if (!TUNABLE_INT_FETCH("hw.ufshci.qcom.phy_mmio_offset",
	    &mmio_offset) || mmio_offset < 0)
		return (false);

	*tag = ctrlr->bus_tag;
	*handle = ctrlr->bus_handle;
	*base_offset = (bus_size_t)mmio_offset;
	return (true);
}

bool
ufshci_ctrlr_qcom_has_phy_mmio(struct ufshci_controller *ctrlr)
{
	bus_space_tag_t tag;
	bus_space_handle_t handle;
	bus_size_t base_offset;

	return (ufshci_ctrlr_qcom_get_phy_mmio(ctrlr, &tag, &handle,
	    &base_offset));
}

static uint32_t
ufshci_ctrlr_qcom_phy_read_4(struct ufshci_controller *ctrlr, bus_size_t reg)
{
	bus_space_tag_t tag;
	bus_space_handle_t handle;
	bus_size_t base_offset;
	bool found;

	found = ufshci_ctrlr_qcom_get_phy_mmio(ctrlr, &tag, &handle,
	    &base_offset);
	KASSERT(found, ("QCOM PHY MMIO is not mapped"));
	if (!found)
		return (0);
	return (bus_space_read_4(tag, handle, base_offset + reg));
}

static void
ufshci_ctrlr_qcom_phy_write_4(struct ufshci_controller *ctrlr, bus_size_t reg,
    uint32_t val)
{
	bus_space_tag_t tag;
	bus_space_handle_t handle;
	bus_size_t base_offset;
	bool found;

	found = ufshci_ctrlr_qcom_get_phy_mmio(ctrlr, &tag, &handle,
	    &base_offset);
	KASSERT(found, ("QCOM PHY MMIO is not mapped"));
	if (!found)
		return;
	bus_space_write_4(tag, handle, base_offset + reg, val);
}

static void
ufshci_ctrlr_qcom_phy_setbits(struct ufshci_controller *ctrlr, bus_size_t reg,
    uint32_t mask)
{
	uint32_t val;

	val = ufshci_ctrlr_qcom_phy_read_4(ctrlr, reg);
	val |= mask;
	ufshci_ctrlr_qcom_phy_write_4(ctrlr, reg, val);
	(void)ufshci_ctrlr_qcom_phy_read_4(ctrlr, reg);
}

static void
ufshci_ctrlr_qcom_phy_clrbits(struct ufshci_controller *ctrlr, bus_size_t reg,
    uint32_t mask)
{
	uint32_t val;

	val = ufshci_ctrlr_qcom_phy_read_4(ctrlr, reg);
	val &= ~mask;
	ufshci_ctrlr_qcom_phy_write_4(ctrlr, reg, val);
	(void)ufshci_ctrlr_qcom_phy_read_4(ctrlr, reg);
}

static void
ufshci_ctrlr_qcom_phy_write_tbl(struct ufshci_controller *ctrlr,
    bus_size_t block_offset, const struct ufshci_qcom_qmp_reg_val *tbl,
    size_t num_entries)
{
	size_t i;

	for (i = 0; i < num_entries; i++)
		ufshci_ctrlr_qcom_phy_write_4(ctrlr, block_offset + tbl[i].reg,
		    tbl[i].val);
}

static int
ufshci_ctrlr_qcom_qmp_ufs_phy_v6_power_on(struct ufshci_controller *ctrlr)
{
	uint32_t ready;
	int timeout_us;

	if (!ufshci_ctrlr_qcom_has_phy_mmio(ctrlr)) {
		ufshci_printf(ctrlr,
		    "QCOM QMP UFS PHY MMIO unavailable, skipping direct PHY bring-up\n");
		return (0);
	}

	/*
	 * Minimal SM8550/X1E80100 QMP UFS PHY v6 bring-up for the current
	 * FreeBSD HS-B / G4 prototype path.
	 */
	ufshci_ctrlr_qcom_phy_clrbits(ctrlr,
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_PHY_START,
	    UFSHCI_QCOM_QMP_PHY_SERDES_START);
	ufshci_ctrlr_qcom_phy_setbits(ctrlr,
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_POWER_DOWN_CONTROL,
	    UFSHCI_QCOM_QMP_PHY_SW_PWRDN);
	ufshci_ctrlr_qcom_phy_setbits(ctrlr,
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_SW_RESET,
	    UFSHCI_QCOM_QMP_PHY_SW_RESET);

	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_SERDES_OFF,
	    ufshci_qcom_sm8550_ufsphy_serdes,
	    nitems(ufshci_qcom_sm8550_ufsphy_serdes));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_SERDES_OFF,
	    ufshci_qcom_sm8550_ufsphy_g4_serdes,
	    nitems(ufshci_qcom_sm8550_ufsphy_g4_serdes));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_SERDES_OFF,
	    ufshci_qcom_sm8550_ufsphy_hs_b_serdes,
	    nitems(ufshci_qcom_sm8550_ufsphy_hs_b_serdes));

	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_TX_OFF,
	    ufshci_qcom_sm8550_ufsphy_tx,
	    nitems(ufshci_qcom_sm8550_ufsphy_tx));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_TX2_OFF,
	    ufshci_qcom_sm8550_ufsphy_tx,
	    nitems(ufshci_qcom_sm8550_ufsphy_tx));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_TX_OFF,
	    ufshci_qcom_sm8550_ufsphy_g4_tx,
	    nitems(ufshci_qcom_sm8550_ufsphy_g4_tx));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_TX2_OFF,
	    ufshci_qcom_sm8550_ufsphy_g4_tx,
	    nitems(ufshci_qcom_sm8550_ufsphy_g4_tx));

	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_RX_OFF,
	    ufshci_qcom_sm8550_ufsphy_rx,
	    nitems(ufshci_qcom_sm8550_ufsphy_rx));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_RX2_OFF,
	    ufshci_qcom_sm8550_ufsphy_rx,
	    nitems(ufshci_qcom_sm8550_ufsphy_rx));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_RX_OFF,
	    ufshci_qcom_sm8550_ufsphy_g4_rx,
	    nitems(ufshci_qcom_sm8550_ufsphy_g4_rx));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_RX2_OFF,
	    ufshci_qcom_sm8550_ufsphy_g4_rx,
	    nitems(ufshci_qcom_sm8550_ufsphy_g4_rx));

	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF,
	    ufshci_qcom_sm8550_ufsphy_pcs,
	    nitems(ufshci_qcom_sm8550_ufsphy_pcs));
	ufshci_ctrlr_qcom_phy_write_tbl(ctrlr, UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF,
	    ufshci_qcom_sm8550_ufsphy_g4_pcs,
	    nitems(ufshci_qcom_sm8550_ufsphy_g4_pcs));

	ufshci_ctrlr_qcom_phy_clrbits(ctrlr,
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_SW_RESET,
	    UFSHCI_QCOM_QMP_PHY_SW_RESET);
	ufshci_ctrlr_qcom_phy_setbits(ctrlr,
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
	    UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_PHY_START,
	    UFSHCI_QCOM_QMP_PHY_SERDES_START);

	for (timeout_us = 0; timeout_us < UFSHCI_QCOM_QMP_PHY_INIT_TIMEOUT_US;
	    timeout_us += UFSHCI_QCOM_QMP_PHY_INIT_POLL_US) {
		ready = ufshci_ctrlr_qcom_phy_read_4(ctrlr,
		    UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
		    UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_READY_STATUS);
		if ((ready & UFSHCI_QCOM_QMP_PHY_PCS_READY) != 0) {
			ufshci_printf(ctrlr, "QCOM QMP UFS PHY v6 ready\n");
			return (0);
		}
		DELAY(UFSHCI_QCOM_QMP_PHY_INIT_POLL_US);
	}

	ufshci_printf(ctrlr,
	    "QCOM QMP UFS PHY v6 init timed out: ready=%#x start=%#x sw_reset=%#x pwrdn=%#x\n",
	    ufshci_ctrlr_qcom_phy_read_4(ctrlr,
		UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
		UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_READY_STATUS),
	    ufshci_ctrlr_qcom_phy_read_4(ctrlr,
		UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
		UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_PHY_START),
	    ufshci_ctrlr_qcom_phy_read_4(ctrlr,
		UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
		UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_SW_RESET),
	    ufshci_ctrlr_qcom_phy_read_4(ctrlr,
		UFSHCI_QCOM_QMP_PHY_V6_PCS_OFF +
		UFSHCI_QCOM_QMP_PHY_V6_PCS_UFS_POWER_DOWN_CONTROL));
	return (ENXIO);
}

static void
ufshci_ctrlr_qcom_select_unipro_mode(struct ufshci_controller *ctrlr)
{
	uint32_t cfg0, cfg1, hw_major;

	cfg1 = bus_space_read_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_CFG1);
	cfg1 |= UFSHCIM(UFSHCI_QCOM_CFG1_REG_QUNIPRO_SEL);
	bus_space_write_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_CFG1, cfg1);
	(void)bus_space_read_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_CFG1);

	hw_major = ufshci_ctrlr_qcom_get_hw_major(ctrlr);
	if (hw_major < 5)
		return;

	cfg0 = bus_space_read_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_CFG0);
	cfg0 &= ~UFSHCIM(UFSHCI_QCOM_CFG0_REG_QUNIPRO_G4_SEL);
	bus_space_write_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_CFG0, cfg0);
	(void)bus_space_read_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_CFG0);
}

static int
ufshci_ctrlr_qcom_set_clk_40ns_cycles(struct ufshci_controller *ctrlr,
    uint32_t cycles_in_1us)
{
	uint32_t cycles_in_40ns, hw_major, reg;

	hw_major = ufshci_ctrlr_qcom_get_hw_major(ctrlr);
	if (hw_major < 4)
		return (0);

	switch (cycles_in_1us) {
	case 38:
		cycles_in_40ns = 2;
		break;
	case 75:
		cycles_in_40ns = 3;
		break;
	case 100:
		cycles_in_40ns = 4;
		break;
	case 150:
		cycles_in_40ns = 6;
		break;
	case 202:
		cycles_in_40ns = 8;
		break;
	case 300:
		cycles_in_40ns = 12;
		break;
	case 403:
		cycles_in_40ns = 16;
		break;
	default:
		ufshci_printf(ctrlr,
		    "unsupported QCOM UniPro core clock %u MHz\n",
		    cycles_in_1us);
		return (EINVAL);
	}

	if (ufshci_uic_send_dme_get(ctrlr, PA_VS_CORE_CLK_40NS_CYCLES, &reg))
		return (ENXIO);

	reg &= ~UFSHCIM(PA_VS_CORE_CLK_40NS_CYCLES_REG_CYCLES);
	reg |= UFSHCIF(PA_VS_CORE_CLK_40NS_CYCLES_REG_CYCLES,
	    cycles_in_40ns);

	if (ufshci_uic_send_dme_set(ctrlr, PA_VS_CORE_CLK_40NS_CYCLES, reg))
		return (ENXIO);

	return (0);
}

static int
ufshci_ctrlr_qcom_pre_link_startup(struct ufshci_controller *ctrlr)
{
	uint32_t core_clk_ctrl_reg, cycles_in_1us, hw_major;
	int acpi_bringup_error, error;

	if (!(ctrlr->quirks & UFSHCI_QUIRK_QCOM_CORE_CLK_300MHZ))
		return (0);

	acpi_bringup_error = ufshci_ctrlr_qcom_prepare_acpi_bringup(ctrlr);

	/*
	 * QCOM24A5-class platforms expose 300MHz max values for both core_clk
	 * and core_clk_unipro in the upstream DT.
	 */
	cycles_in_1us = UFSHCI_QCOM_CORE_CLK_300MHZ;
	hw_major = ufshci_ctrlr_qcom_get_hw_major(ctrlr);

	ufshci_ctrlr_qcom_select_unipro_mode(ctrlr);

	bus_space_write_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_SYS1CLK_1US, cycles_in_1us);
	(void)bus_space_read_4(ctrlr->bus_tag, ctrlr->bus_handle,
	    UFSHCI_QCOM_REG_SYS1CLK_1US);

	if (ufshci_uic_send_dme_get(ctrlr, DME_VS_CORE_CLK_CTRL,
	    &core_clk_ctrl_reg))
		return (ENXIO);

	if (hw_major >= 4) {
		core_clk_ctrl_reg &=
		    ~UFSHCIM(DME_VS_CORE_CLK_CTRL_REG_CLK_1US_CYCLES_V4);
		core_clk_ctrl_reg |=
		    UFSHCIF(DME_VS_CORE_CLK_CTRL_REG_CLK_1US_CYCLES_V4,
			cycles_in_1us);
	} else {
		core_clk_ctrl_reg &=
		    ~UFSHCIM(DME_VS_CORE_CLK_CTRL_REG_CLK_1US_CYCLES);
		core_clk_ctrl_reg |=
		    UFSHCIF(DME_VS_CORE_CLK_CTRL_REG_CLK_1US_CYCLES,
			cycles_in_1us);
	}

	core_clk_ctrl_reg &=
	    ~UFSHCIM(DME_VS_CORE_CLK_CTRL_REG_CORE_CLK_DIV_EN);

	if (ufshci_uic_send_dme_set(ctrlr, DME_VS_CORE_CLK_CTRL,
	    core_clk_ctrl_reg))
		return (ENXIO);

	error = ufshci_ctrlr_qcom_set_clk_40ns_cycles(ctrlr, cycles_in_1us);
	if (error != 0)
		return (error);
	if (acpi_bringup_error != 0)
		return (0);

	return (ufshci_ctrlr_qcom_qmp_ufs_phy_v6_power_on(ctrlr));
}

/* Some controllers require a reinit after switching to the max gear. */
static int
ufshci_ctrlr_reinit_after_max_gear_switch(struct ufshci_controller *ctrlr)
{
	int error;

	/* Reset device */
	ufshci_utmr_req_queue_disable(ctrlr);
	ufshci_utr_req_queue_disable(ctrlr);

	error = ufshci_ctrlr_disable(ctrlr);
	if (error != 0)
		return (error);

	error = ufshci_ctrlr_enable(ctrlr);
	if (error != 0)
		return (error);

	error = ufshci_utmr_req_queue_enable(ctrlr);
	if (error != 0)
		return (error);

	error = ufshci_utr_req_queue_enable(ctrlr);
	if (error != 0)
		return (error);

	error = ufshci_ctrlr_send_nop(ctrlr);
	if (error != 0)
		return (error);

	/* Reinit the target device. */
	error = ufshci_dev_init(ctrlr);
	if (error != 0)
		return (error);

	/* Initialize Reference Clock */
	error = ufshci_dev_init_reference_clock(ctrlr);
	if (error != 0)
		return (error);

	/* Initialize unipro */
	error = ufshci_dev_init_unipro(ctrlr);
	if (error != 0)
		return (error);

	if (!(ctrlr->quirks & UFSHCI_QUIRK_IGNORE_UIC_POWER_MODE)) {
		error = ufshci_dev_init_uic_power_mode(ctrlr);
		if (error != 0)
			return (error);
		ufshci_dev_init_uic_link_state(ctrlr);
	}

	return (0);
}

static void
ufshci_ctrlr_start(struct ufshci_controller *ctrlr, bool resetting)
{
	TSENTER();

	/*
	 * If `resetting` is true, we are on the reset path.
	 * Re-enable request queues here because ufshci_ctrlr_reset_task()
	 * disables them during reset.
	 */
	if (resetting) {
		if (ufshci_utmr_req_queue_enable(ctrlr) != 0) {
			ufshci_ctrlr_fail(ctrlr);
			return;
		}
		if (ufshci_utr_req_queue_enable(ctrlr) != 0) {
			ufshci_ctrlr_fail(ctrlr);
			return;
		}
	}

	if (ufshci_ctrlr_send_nop(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	/* Initialize UFS target drvice */
	if (ufshci_dev_init(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	/*
	 * Read device parameters before HS power mode negotiation so any
	 * descriptor-based device handling is available while the link is
	 * still in its initial mode.
	 */
	if (ufshci_dev_get_descriptor(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	/* Initialize Reference Clock */
	if (ufshci_dev_init_reference_clock(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	/* Initialize unipro */
	if (ufshci_dev_init_unipro(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	/*
	 * Initialize UIC Power Mode
	 * QEMU UFS devices do not support unipro and power mode.
	 */
	if (!(ctrlr->quirks & UFSHCI_QUIRK_IGNORE_UIC_POWER_MODE) &&
	    ufshci_dev_init_uic_power_mode(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	ufshci_dev_init_uic_link_state(ctrlr);

	if ((ctrlr->quirks & UFSHCI_QUIRK_REINIT_AFTER_MAX_GEAR_SWITCH) &&
	    ctrlr->hs_gear != 0 &&
	    ufshci_ctrlr_reinit_after_max_gear_switch(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	if (ufshci_dev_config_write_booster(ctrlr)) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	ufshci_dev_init_auto_hibernate(ctrlr);

	/* TODO: Configure Write Protect */

	/* TODO: Configure Background Operations */

	/*
	 * If the reset is due to a timeout, it is already attached to the SIM
	 * and does not need to be attached again.
	 */
	if (!resetting && ufshci_sim_attach(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	/* Initialize UFS Power Mode */
	if (ufshci_dev_init_ufs_power_mode(ctrlr) != 0) {
		ufshci_ctrlr_fail(ctrlr);
		return;
	}

	TSEXIT();
}

static int
ufshci_ctrlr_disable_host_ctrlr(struct ufshci_controller *ctrlr)
{
	int timeout = ticks + MSEC_2_TICKS(ctrlr->device_init_timeout_in_ms);
	sbintime_t delta_t = SBT_1US;
	uint32_t hce;

	hce = ufshci_mmio_read_4(ctrlr, hce);

	/* If UFS host controller is already enabled, disable it. */
	if (UFSHCIV(UFSHCI_HCE_REG_HCE, hce)) {
		hce &= ~UFSHCIM(UFSHCI_HCE_REG_HCE);
		ufshci_mmio_write_4(ctrlr, hce, hce);
	}

	/* Wait for the HCE flag to change */
	while (1) {
		hce = ufshci_mmio_read_4(ctrlr, hce);
		if (!UFSHCIV(UFSHCI_HCE_REG_HCE, hce))
			break;
		if (timeout - ticks < 0) {
			ufshci_printf(ctrlr,
			    "host controller failed to disable "
			    "within %d ms\n",
			    ctrlr->device_init_timeout_in_ms);
			return (ENXIO);
		}

		pause_sbt("ufshci_disable_hce", delta_t, 0, C_PREL(1));
		delta_t = min(SBT_1MS, delta_t * 3 / 2);
	}

	return (0);
}

static int
ufshci_ctrlr_enable_host_ctrlr(struct ufshci_controller *ctrlr)
{
	int timeout = ticks + MSEC_2_TICKS(ctrlr->device_init_timeout_in_ms);
	sbintime_t delta_t = SBT_1US;
	uint32_t hce;

	hce = ufshci_mmio_read_4(ctrlr, hce);

	/* Enable UFS host controller */
	hce |= UFSHCIM(UFSHCI_HCE_REG_HCE);
	ufshci_mmio_write_4(ctrlr, hce, hce);

	/*
	 * During the controller initialization, the value of the HCE bit is
	 * unstable, so we need to read the HCE value after some time after
	 * initialization is complete.
	 */
	pause_sbt("ufshci_enable_hce", ustosbt(100), 0, C_PREL(1));

	/* Wait for the HCE flag to change */
	while (1) {
		hce = ufshci_mmio_read_4(ctrlr, hce);
		if (UFSHCIV(UFSHCI_HCE_REG_HCE, hce))
			break;
		if (timeout - ticks < 0) {
			ufshci_printf(ctrlr,
			    "host controller failed to enable "
			    "within %d ms\n",
			    ctrlr->device_init_timeout_in_ms);
			return (ENXIO);
		}

		pause_sbt("ufshci_enable_hce", delta_t, 0, C_PREL(1));
		delta_t = min(SBT_1MS, delta_t * 3 / 2);
	}

	return (0);
}

int
ufshci_ctrlr_disable(struct ufshci_controller *ctrlr)
{
	int error;

	/* Disable all interrupts */
	ufshci_mmio_write_4(ctrlr, ie, 0);

	error = ufshci_ctrlr_disable_host_ctrlr(ctrlr);
	return (error);
}

int
ufshci_ctrlr_enable(struct ufshci_controller *ctrlr)
{
	uint32_t ie, hcs;
	int error;

	error = ufshci_ctrlr_enable_host_ctrlr(ctrlr);
	if (error)
		return (error);

	error = ufshci_ctrlr_qcom_pre_link_startup(ctrlr);
	if (error)
		return (error);

	if (ctrlr->quirks & UFSHCI_QUIRK_DISABLE_HOST_TX_LCC) {
		error = ufshci_uic_send_dme_set(ctrlr, PA_LocalTxLCCEnable, 0);
		if (error)
			return (error);
	}

	/* Send DME_LINKSTARTUP command to start the link startup procedure */
	error = ufshci_uic_send_dme_link_startup(ctrlr);
	if (error)
		return (error);

	/*
	 * The device_present(UFSHCI_HCS_REG_DP) bit becomes true if the host
	 * controller has successfully received a Link Startup UIC command
	 * response and the UFS device has found a physical link to the
	 * controller.
	 */
	hcs = ufshci_mmio_read_4(ctrlr, hcs);
	if (!UFSHCIV(UFSHCI_HCS_REG_DP, hcs)) {
		ufshci_printf(ctrlr, "UFS device not found\n");
		return (ENXIO);
	}

	/* Enable additional interrupts by programming the IE register. */
	ie = ufshci_mmio_read_4(ctrlr, ie);
	ie |= UFSHCIM(UFSHCI_IE_REG_UTRCE);  /* UTR Completion */
	ie |= UFSHCIM(UFSHCI_IE_REG_UEE);    /* UIC Error */
	ie |= UFSHCIM(UFSHCI_IE_REG_UTMRCE); /* UTMR Completion */
	ie |= UFSHCIM(UFSHCI_IE_REG_DFEE);   /* Device Fatal Error */
	ie |= UFSHCIM(UFSHCI_IE_REG_UTPEE);  /* UTP Error */
	ie |= UFSHCIM(UFSHCI_IE_REG_HCFEE);  /* Host Ctrlr Fatal Error */
	ie |= UFSHCIM(UFSHCI_IE_REG_SBFEE);  /* System Bus Fatal Error */
	ie |= UFSHCIM(UFSHCI_IE_REG_CEFEE);  /* Crypto Engine Fatal Error */
	ufshci_mmio_write_4(ctrlr, ie, ie);

	/* TODO: Initialize interrupt Aggregation Control Register (UTRIACR) */

	return (0);
}

static int
ufshci_ctrlr_hw_reset(struct ufshci_controller *ctrlr)
{
	int error;

	error = ufshci_ctrlr_disable(ctrlr);
	if (error)
		return (error);

	error = ufshci_ctrlr_enable(ctrlr);
	return (error);
}

static void
ufshci_ctrlr_reset_task(void *arg, int pending)
{
	struct ufshci_controller *ctrlr = arg;
	int error;

	/* Release resources */
	ufshci_utmr_req_queue_disable(ctrlr);
	ufshci_utr_req_queue_disable(ctrlr);

	error = ufshci_ctrlr_hw_reset(ctrlr);
	if (error)
		return (ufshci_ctrlr_fail(ctrlr));

	ufshci_ctrlr_start(ctrlr, true);
}

int
ufshci_ctrlr_construct(struct ufshci_controller *ctrlr, device_t dev)
{
	uint32_t ver, cap, ahit;
	uint32_t timeout_period, retry_count;
	int error;

	ctrlr->device_init_timeout_in_ms = UFSHCI_DEVICE_INIT_TIMEOUT_MS;
	ctrlr->uic_cmd_timeout_in_ms = UFSHCI_UIC_CMD_TIMEOUT_MS;
	ctrlr->dev = dev;
	ctrlr->sc_unit = device_get_unit(dev);

	snprintf(ctrlr->sc_name, sizeof(ctrlr->sc_name), "%s",
	    device_get_nameunit(dev));

	mtx_init(&ctrlr->sc_mtx, device_get_nameunit(dev), NULL,
	    MTX_DEF | MTX_RECURSE);

	mtx_init(&ctrlr->uic_cmd_lock, "ufshci ctrlr uic cmd lock", NULL,
	    MTX_DEF);

	ver = ufshci_mmio_read_4(ctrlr, ver);
	ctrlr->major_version = UFSHCIV(UFSHCI_VER_REG_MJR, ver);
	ctrlr->minor_version = UFSHCIV(UFSHCI_VER_REG_MNR, ver);
	ufshci_printf(ctrlr, "UFSHCI Version: %d.%d\n", ctrlr->major_version,
	    ctrlr->minor_version);

	/* Read Device Capabilities */
	ctrlr->cap = cap = ufshci_mmio_read_4(ctrlr, cap);
	if (ctrlr->quirks & UFSHCI_QUIRK_BROKEN_LSDBS_MCQS_CAP) {
		ctrlr->is_single_db_supported = true;
		ctrlr->is_mcq_supported = true;
	} else {
		ctrlr->is_single_db_supported = (UFSHCIV(UFSHCI_CAP_REG_LSDBS,
						     cap) == 0);
		ctrlr->is_mcq_supported = (UFSHCIV(UFSHCI_CAP_REG_MCQS, cap) ==
		    1);
	}
	if (!(ctrlr->is_single_db_supported || ctrlr->is_mcq_supported))
		return (ENXIO);

	/*
	 * The maximum transfer size supported by UFSHCI spec is 65535 * 256 KiB
	 * However, we limit the maximum transfer size to 1MiB(256 * 4KiB) for
	 * performance reason.
	 */
	ctrlr->page_size = PAGE_SIZE;
	ctrlr->max_xfer_size = ctrlr->page_size * UFSHCI_MAX_PRDT_ENTRY_COUNT;

	timeout_period = UFSHCI_DEFAULT_TIMEOUT_PERIOD;
	TUNABLE_INT_FETCH("hw.ufshci.timeout_period", &timeout_period);
	timeout_period = min(timeout_period, UFSHCI_MAX_TIMEOUT_PERIOD);
	timeout_period = max(timeout_period, UFSHCI_MIN_TIMEOUT_PERIOD);
	ctrlr->timeout_period = timeout_period;

	retry_count = UFSHCI_DEFAULT_RETRY_COUNT;
	TUNABLE_INT_FETCH("hw.ufshci.retry_count", &retry_count);
	ctrlr->retry_count = retry_count;

	ctrlr->enable_aborts = 1;
	if (ctrlr->quirks & UFSHCI_QUIRK_NOT_SUPPORT_ABORT_TASK)
		ctrlr->enable_aborts = 0;
	else
		TUNABLE_INT_FETCH("hw.ufshci.enable_aborts",
		    &ctrlr->enable_aborts);

	/* Reset the UFSHCI controller */
	error = ufshci_ctrlr_hw_reset(ctrlr);
	if (error)
		return (error);

	/* Read the UECPA register to clear */
	ufshci_mmio_read_4(ctrlr, uecpa);

	/* Diable Auto-hibernate */
	ahit = 0;
	ufshci_mmio_write_4(ctrlr, ahit, ahit);

	/* Allocate and initialize UTP Task Management Request List. */
	error = ufshci_utmr_req_queue_construct(ctrlr);
	if (error)
		return (error);

	/* Allocate and initialize UTP Transfer Request List or SQ/CQ. */
	error = ufshci_utr_req_queue_construct(ctrlr);
	if (error)
		return (error);

	/* TODO: Separate IO and Admin slot */

	/*
	 * max_hw_pend_io is the number of slots in the transfer_req_queue.
	 * Reduce num_entries by one to reserve an admin slot.
	 */
	ctrlr->max_hw_pend_io = ctrlr->transfer_req_queue.num_entries - 1;

	/* Create a thread for the taskqueue. */
	ctrlr->taskqueue = taskqueue_create("ufshci_taskq", M_WAITOK,
	    taskqueue_thread_enqueue, &ctrlr->taskqueue);
	taskqueue_start_threads(&ctrlr->taskqueue, 1, PI_DISK, "ufshci taskq");

	TASK_INIT(&ctrlr->reset_task, 0, ufshci_ctrlr_reset_task, ctrlr);

	return (0);
}

void
ufshci_ctrlr_destruct(struct ufshci_controller *ctrlr, device_t dev)
{
	if (ctrlr->resource == NULL)
		goto nores;

	/* TODO: Flush In-flight IOs */

	/* Release resources */
	ufshci_utmr_req_queue_destroy(ctrlr);
	ufshci_utr_req_queue_destroy(ctrlr);

	if (ctrlr->tag)
		bus_teardown_intr(ctrlr->dev, ctrlr->res, ctrlr->tag);

	if (ctrlr->res)
		bus_release_resource(ctrlr->dev, SYS_RES_IRQ,
		    rman_get_rid(ctrlr->res), ctrlr->res);

	mtx_lock(&ctrlr->sc_mtx);

	ufshci_sim_detach(ctrlr);

	mtx_unlock(&ctrlr->sc_mtx);

	bus_release_resource(dev, SYS_RES_MEMORY, ctrlr->resource_id,
	    ctrlr->resource);
nores:
	KASSERT(!mtx_owned(&ctrlr->uic_cmd_lock),
	    ("destroying uic_cmd_lock while still owned"));
	mtx_destroy(&ctrlr->uic_cmd_lock);

	KASSERT(!mtx_owned(&ctrlr->sc_mtx),
	    ("destroying sc_mtx while still owned"));
	mtx_destroy(&ctrlr->sc_mtx);

	return;
}

void
ufshci_ctrlr_reset(struct ufshci_controller *ctrlr)
{
	taskqueue_enqueue(ctrlr->taskqueue, &ctrlr->reset_task);
}

int
ufshci_ctrlr_submit_task_mgmt_request(struct ufshci_controller *ctrlr,
    struct ufshci_request *req)
{
	return (
	    ufshci_req_queue_submit_request(&ctrlr->task_mgmt_req_queue, req));
}

int
ufshci_ctrlr_submit_transfer_request(struct ufshci_controller *ctrlr,
    struct ufshci_request *req)
{
	return (
	    ufshci_req_queue_submit_request(&ctrlr->transfer_req_queue, req));
}

int
ufshci_ctrlr_send_nop(struct ufshci_controller *ctrlr)
{
	struct ufshci_completion_poll_status status;

	status.done = 0;
	ufshci_ctrlr_cmd_send_nop(ctrlr, ufshci_completion_poll_cb, &status);
	ufshci_completion_poll(&status);
	if (status.error) {
		ufshci_printf(ctrlr, "ufshci_ctrlr_send_nop failed!\n");
		return (ENXIO);
	}

	return (0);
}

void
ufshci_ctrlr_start_config_hook(void *arg)
{
	struct ufshci_controller *ctrlr = arg;

	TSENTER();

	if (ufshci_utmr_req_queue_enable(ctrlr) == 0 &&
	    ufshci_utr_req_queue_enable(ctrlr) == 0)
		ufshci_ctrlr_start(ctrlr, false);
	else
		ufshci_ctrlr_fail(ctrlr);

	ufshci_sysctl_initialize_ctrlr(ctrlr);
	config_intrhook_disestablish(&ctrlr->config_hook);

	TSEXIT();
}

/*
 * Poll all the queues enabled on the device for completion.
 */
void
ufshci_ctrlr_poll(struct ufshci_controller *ctrlr)
{
	uint32_t is;

	is = ufshci_mmio_read_4(ctrlr, is);

	/* UIC error */
	if (is & UFSHCIM(UFSHCI_IS_REG_UE)) {
		uint32_t uecpa, uecdl, uecn, uect, uecdme;

		/* UECPA for Host UIC Error Code within PHY Adapter Layer */
		uecpa = ufshci_mmio_read_4(ctrlr, uecpa);
		if (uecpa & UFSHCIM(UFSHCI_UECPA_REG_ERR)) {
			ufshci_printf(ctrlr, "UECPA error code: 0x%x\n",
			    UFSHCIV(UFSHCI_UECPA_REG_EC, uecpa));
		}
		/* UECDL for Host UIC Error Code within Data Link Layer */
		uecdl = ufshci_mmio_read_4(ctrlr, uecdl);
		if (uecdl & UFSHCIM(UFSHCI_UECDL_REG_ERR)) {
			ufshci_printf(ctrlr, "UECDL error code: 0x%x\n",
			    UFSHCIV(UFSHCI_UECDL_REG_EC, uecdl));
		}
		/* UECN for Host UIC Error Code within Network Layer */
		uecn = ufshci_mmio_read_4(ctrlr, uecn);
		if (uecn & UFSHCIM(UFSHCI_UECN_REG_ERR)) {
			ufshci_printf(ctrlr, "UECN error code: 0x%x\n",
			    UFSHCIV(UFSHCI_UECN_REG_EC, uecn));
		}
		/* UECT for Host UIC Error Code within Transport Layer */
		uect = ufshci_mmio_read_4(ctrlr, uect);
		if (uect & UFSHCIM(UFSHCI_UECT_REG_ERR)) {
			ufshci_printf(ctrlr, "UECT error code: 0x%x\n",
			    UFSHCIV(UFSHCI_UECT_REG_EC, uect));
		}
		/* UECDME for Host UIC Error Code within DME subcomponent */
		uecdme = ufshci_mmio_read_4(ctrlr, uecdme);
		if (uecdme & UFSHCIM(UFSHCI_UECDME_REG_ERR)) {
			ufshci_printf(ctrlr, "UECDME error code: 0x%x\n",
			    UFSHCIV(UFSHCI_UECDME_REG_EC, uecdme));
		}
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_UE));
	}
	/* Device Fatal Error Status */
	if (is & UFSHCIM(UFSHCI_IS_REG_DFES)) {
		ufshci_printf(ctrlr, "Device fatal error on ISR\n");
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_DFES));
	}
	/* UTP Error Status */
	if (is & UFSHCIM(UFSHCI_IS_REG_UTPES)) {
		ufshci_printf(ctrlr, "UTP error on ISR\n");
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_UTPES));
	}
	/* Host Controller Fatal Error Status */
	if (is & UFSHCIM(UFSHCI_IS_REG_HCFES)) {
		ufshci_printf(ctrlr, "Host controller fatal error on ISR\n");
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_HCFES));
	}
	/* System Bus Fatal Error Status */
	if (is & UFSHCIM(UFSHCI_IS_REG_SBFES)) {
		ufshci_printf(ctrlr, "System bus fatal error on ISR\n");
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_SBFES));
	}
	/* Crypto Engine Fatal Error Status */
	if (is & UFSHCIM(UFSHCI_IS_REG_CEFES)) {
		ufshci_printf(ctrlr, "Crypto engine fatal error on ISR\n");
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_CEFES));
	}
	/* UTP Task Management Request Completion Status */
	if (is & UFSHCIM(UFSHCI_IS_REG_UTMRCS)) {
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_UTMRCS));
		ufshci_req_queue_process_completions(
		    &ctrlr->task_mgmt_req_queue);
	}
	/* UTP Transfer Request Completion Status */
	if (is & UFSHCIM(UFSHCI_IS_REG_UTRCS)) {
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_UTRCS));
		ufshci_req_queue_process_completions(
		    &ctrlr->transfer_req_queue);
	}
	/* MCQ CQ Event Status */
	if (is & UFSHCIM(UFSHCI_IS_REG_CQES)) {
		/* TODO: We need to process completion Queue Pairs */
		ufshci_printf(ctrlr, "MCQ completion not yet implemented\n");
		ufshci_mmio_write_4(ctrlr, is, UFSHCIM(UFSHCI_IS_REG_CQES));
	}
}

/*
 * Poll the single-vector interrupt case: num_io_queues will be 1 and
 * there's only a single vector. While we're polling, we mask further
 * interrupts in the controller.
 */
void
ufshci_ctrlr_shared_handler(void *arg)
{
	struct ufshci_controller *ctrlr = arg;

	ufshci_ctrlr_poll(ctrlr);
}

void
ufshci_reg_dump(struct ufshci_controller *ctrlr)
{
	ufshci_printf(ctrlr, "========= UFSHCI Register Dump =========\n");

	UFSHCI_DUMP_REG(ctrlr, cap);
	UFSHCI_DUMP_REG(ctrlr, mcqcap);
	UFSHCI_DUMP_REG(ctrlr, ver);
	UFSHCI_DUMP_REG(ctrlr, ext_cap);
	UFSHCI_DUMP_REG(ctrlr, hcpid);
	UFSHCI_DUMP_REG(ctrlr, hcmid);
	UFSHCI_DUMP_REG(ctrlr, ahit);
	UFSHCI_DUMP_REG(ctrlr, is);
	UFSHCI_DUMP_REG(ctrlr, ie);
	UFSHCI_DUMP_REG(ctrlr, hcsext);
	UFSHCI_DUMP_REG(ctrlr, hcs);
	UFSHCI_DUMP_REG(ctrlr, hce);
	UFSHCI_DUMP_REG(ctrlr, uecpa);
	UFSHCI_DUMP_REG(ctrlr, uecdl);
	UFSHCI_DUMP_REG(ctrlr, uecn);
	UFSHCI_DUMP_REG(ctrlr, uect);
	UFSHCI_DUMP_REG(ctrlr, uecdme);

	ufshci_printf(ctrlr, "========================================\n");
}

int
ufshci_ctrlr_suspend(struct ufshci_controller *ctrlr, enum power_stype stype)
{
	int error;

	if (!ctrlr->ufs_dev.power_mode_supported)
		return (0);

	/* TODO: Need to flush the request queue */

	if (ctrlr->ufs_device_wlun_periph) {
		ctrlr->ufs_dev.power_mode = power_map[stype].dev_pwr;
		error = ufshci_sim_send_ssu(ctrlr, /*start*/ false,
		    power_map[stype].ssu_pc, /*immed*/ false);
		if (error) {
			ufshci_printf(ctrlr,
			    "Failed to send SSU in suspend handler\n");
			return (error);
		}
	}

	/* Change the link state */
	error = ufshci_dev_link_state_transition(ctrlr,
	    power_map[stype].link_state);
	if (error) {
		ufshci_printf(ctrlr,
		    "Failed to transition link state in suspend handler\n");
		return (error);
	}

	return (0);
}

int
ufshci_ctrlr_resume(struct ufshci_controller *ctrlr, enum power_stype stype)
{
	int error;

	if (!ctrlr->ufs_dev.power_mode_supported)
		return (0);

	/* Change the link state */
	error = ufshci_dev_link_state_transition(ctrlr,
	    power_map[stype].link_state);
	if (error) {
		ufshci_printf(ctrlr,
		    "Failed to transition link state in resume handler\n");
		return (error);
	}

	if (ctrlr->ufs_device_wlun_periph) {
		ctrlr->ufs_dev.power_mode = power_map[stype].dev_pwr;
		error = ufshci_sim_send_ssu(ctrlr, /*start*/ false,
		    power_map[stype].ssu_pc, /*immed*/ false);
		if (error) {
			ufshci_printf(ctrlr,
			    "Failed to send SSU in resume handler\n");
			return (error);
		}
	}

	ufshci_dev_enable_auto_hibernate(ctrlr);

	return (0);
}
