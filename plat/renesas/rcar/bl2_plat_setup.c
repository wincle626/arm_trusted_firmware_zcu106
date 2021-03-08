/*
 * Copyright (c) 2018, Renesas Electronics Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <desc_image_load.h>
#include <arch_helpers.h>
#include <bl_common.h>
#include <bl1.h>
#include <console.h>
#include <debug.h>
#include <mmio.h>
#include <platform.h>
#include <platform_def.h>
#include <string.h>

#include "avs_driver.h"
#include "boot_init_dram.h"
#include "cpg_registers.h"
#include "board.h"
#include "emmc_def.h"
#include "emmc_hal.h"
#include "emmc_std.h"

#if PMIC_ROHM_BD9571 && RCAR_SYSTEM_RESET_KEEPON_DDR
#include "iic_dvfs.h"
#endif

#include "io_common.h"
#include "qos_init.h"
#include "rcar_def.h"
#include "rcar_private.h"
#include "rcar_version.h"
#include "rom_api.h"

IMPORT_SYM(unsigned long, __RO_START__, BL2_RO_BASE)
IMPORT_SYM(unsigned long, __RO_END__, BL2_RO_LIMIT)

#if USE_COHERENT_MEM
IMPORT_SYM(unsigned long, __COHERENT_RAM_START__, BL2_COHERENT_RAM_BASE)
IMPORT_SYM(unsigned long, __COHERENT_RAM_END__, BL2_COHERENT_RAM_LIMIT)
#endif

extern void plat_rcar_gic_driver_init(void);
extern void plat_rcar_gic_init(void);
extern void bl2_enter_bl31(const struct entry_point_info *bl_ep_info);
extern void bl2_system_cpg_init(void);
extern void bl2_secure_setting(void);
extern void bl2_cpg_init(void);
extern void rcar_io_emmc_setup(void);
extern void rcar_io_setup(void);
extern void rcar_swdt_release(void);
extern void rcar_swdt_init(void);
extern void rcar_rpc_init(void);
extern void rcar_pfc_init(void);
extern void rcar_dma_init(void);

/* R-Car Gen3 product check */
#if (RCAR_LSI == RCAR_H3) || (RCAR_LSI == RCAR_H3N)
#define TARGET_PRODUCT			RCAR_PRODUCT_H3
#define TARGET_NAME			"R-Car H3"
#elif RCAR_LSI == RCAR_M3
#define TARGET_PRODUCT			RCAR_PRODUCT_M3
#define TARGET_NAME			"R-Car M3"
#elif RCAR_LSI == RCAR_M3N
#define TARGET_PRODUCT			RCAR_PRODUCT_M3N
#define TARGET_NAME			"R-Car M3N"
#elif RCAR_LSI == RCAR_E3
#define TARGET_PRODUCT			RCAR_PRODUCT_E3
#define TARGET_NAME			"R-Car E3"
#endif

#if (RCAR_LSI == RCAR_E3)
#define GPIO_INDT			(GPIO_INDT6)
#define GPIO_BKUP_TRG_SHIFT		((uint32_t)1U<<13U)
#else
#define GPIO_INDT			(GPIO_INDT1)
#define GPIO_BKUP_TRG_SHIFT		((uint32_t)1U<<8U)
#endif

CASSERT((PARAMS_BASE + sizeof(bl2_to_bl31_params_mem_t) + 0x100)
	 < (RCAR_SHARED_MEM_BASE + RCAR_SHARED_MEM_SIZE),
	assert_bl31_params_do_not_fit_in_shared_memory);

static meminfo_t bl2_tzram_layout __aligned(CACHE_WRITEBACK_GRANULE);

#if (RCAR_LOSSY_ENABLE == 1)
typedef struct bl2_lossy_info {
	uint32_t magic;
	uint32_t a0;
	uint32_t b0;
} bl2_lossy_info_t;

static void bl2_lossy_setting(uint32_t no, uint64_t start_addr,
			      uint64_t end_addr, uint32_t format,
			      uint32_t enable)
{
	bl2_lossy_info_t info;
	uint32_t reg;

	reg = format | (start_addr >> 20);
	mmio_write_32(AXI_DCMPAREACRA0 + 0x8 * no, reg);
	mmio_write_32(AXI_DCMPAREACRB0 + 0x8 * no, end_addr >> 20);
	mmio_write_32(AXI_DCMPAREACRA0 + 0x8 * no, reg | enable);

	info.magic = 0x12345678U;
	info.a0 = mmio_read_32(AXI_DCMPAREACRA0 + 0x8 * no);
	info.b0 = mmio_read_32(AXI_DCMPAREACRB0 + 0x8 * no);

	mmio_write_32(LOSSY_PARAMS_BASE + sizeof(info) * no, info.magic);
	mmio_write_32(LOSSY_PARAMS_BASE + sizeof(info) * no + 0x4, info.a0);
	mmio_write_32(LOSSY_PARAMS_BASE + sizeof(info) * no + 0x8, info.b0);

	NOTICE("     Entry %d: DCMPAREACRAx:0x%x DCMPAREACRBx:0x%x\n", no,
	       mmio_read_32(AXI_DCMPAREACRA0 + 0x8 * no),
	       mmio_read_32(AXI_DCMPAREACRB0 + 0x8 * no));
}
#endif

void bl2_plat_flush_bl31_params(void)
{
	uint32_t product_cut, product, cut;
	uint32_t boot_dev, boot_cpu;
	uint32_t lcs, reg, val;

	reg = mmio_read_32(RCAR_MODEMR);
	boot_dev = reg & MODEMR_BOOT_DEV_MASK;

	if (boot_dev == MODEMR_BOOT_DEV_EMMC_25X1 ||
	    boot_dev == MODEMR_BOOT_DEV_EMMC_50X8)
		emmc_terminate();

	if ((reg & MODEMR_BOOT_CPU_MASK) != MODEMR_BOOT_CPU_CR7)
		bl2_secure_setting();

	reg = mmio_read_32(RCAR_PRR);
	product_cut = reg & (RCAR_PRODUCT_MASK | RCAR_CUT_MASK);
	product = reg & RCAR_PRODUCT_MASK;
	cut = reg & RCAR_CUT_MASK;

	if (product == RCAR_PRODUCT_M3)
		goto tlb;

	if (product == RCAR_PRODUCT_H3 && RCAR_CUT_VER20 > cut)
		goto tlb;

	/* Disable MFIS write protection */
	mmio_write_32(MFISWPCNTR, MFISWPCNTR_PASSWORD | 1);

tlb:
	reg = mmio_read_32(RCAR_MODEMR);
	boot_cpu = reg & MODEMR_BOOT_CPU_MASK;
	if (boot_cpu != MODEMR_BOOT_CPU_CA57 &&
	    boot_cpu != MODEMR_BOOT_CPU_CA53)
		goto mmu;

	if (product_cut == RCAR_PRODUCT_H3_CUT20) {
		mmio_write_32(IPMMUVI0_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUVI1_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUPV0_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUPV1_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUPV2_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUPV3_IMSCTLR, IMSCTLR_DISCACHE);
	} else if (product_cut == (RCAR_PRODUCT_M3N | RCAR_CUT_VER10) ||
		   product_cut == (RCAR_PRODUCT_M3N | RCAR_CUT_VER11)) {
		mmio_write_32(IPMMUVI0_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUPV0_IMSCTLR, IMSCTLR_DISCACHE);
	} else if (product_cut == (RCAR_PRODUCT_E3 | RCAR_CUT_VER10)) {
		mmio_write_32(IPMMUVI0_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUPV0_IMSCTLR, IMSCTLR_DISCACHE);
	}

	if (product_cut == (RCAR_PRODUCT_H3_CUT20) ||
	    product_cut == (RCAR_PRODUCT_M3N | RCAR_CUT_VER10) ||
	    product_cut == (RCAR_PRODUCT_M3N | RCAR_CUT_VER11) ||
	    product_cut == (RCAR_PRODUCT_E3 | RCAR_CUT_VER10)) {
		mmio_write_32(IPMMUHC_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMURT_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUMP_IMSCTLR, IMSCTLR_DISCACHE);

		mmio_write_32(IPMMUDS0_IMSCTLR, IMSCTLR_DISCACHE);
		mmio_write_32(IPMMUDS1_IMSCTLR, IMSCTLR_DISCACHE);
	}

mmu:
	mmio_write_32(IPMMUMM_IMSCTLR, IPMMUMM_IMSCTLR_ENABLE);
	mmio_write_32(IPMMUMM_IMAUXCTLR, IPMMUMM_IMAUXCTLR_NMERGE40_BIT);

	val = rcar_rom_get_lcs(&lcs);
	if (val) {
		ERROR("BL2: Failed to get the LCS. (%d)\n", val);
		panic();
	}

	if (lcs == LCS_SE)
		mmio_clrbits_32(P_ARMREG_SEC_CTRL, P_ARMREG_SEC_CTRL_PROT);

	rcar_swdt_release();
	bl2_system_cpg_init();

#if RCAR_BL2_DCACHE == 1
	/* Disable data cache (clean and invalidate) */
	disable_mmu_el3();
#endif
}

static uint32_t is_ddr_backup_mode(void)
{
#if RCAR_SYSTEM_SUSPEND
	static uint32_t reason = RCAR_COLD_BOOT;
	static uint32_t once;

#if PMIC_ROHM_BD9571 && RCAR_SYSTEM_RESET_KEEPON_DDR
	uint8_t data;
#endif
	if (once)
		return reason;

	once = 1;
	if ((mmio_read_32(GPIO_INDT) & GPIO_BKUP_TRG_SHIFT) == 0)
		return reason;

#if PMIC_ROHM_BD9571 && RCAR_SYSTEM_RESET_KEEPON_DDR
	if (rcar_iic_dvfs_receive(PMIC, REG_KEEP10, &data)) {
		ERROR("BL2: REG Keep10 READ ERROR.\n");
		panic();
	}

	if (KEEP10_MAGIC != data)
		reason = RCAR_WARM_BOOT;
#else
	reason = RCAR_WARM_BOOT;
#endif
	return reason;
#else
	return RCAR_COLD_BOOT;
#endif
}

int bl2_plat_handle_pre_image_load(unsigned int image_id)
{
	u_register_t *boot_kind = (void *) BOOT_KIND_BASE;
	bl_mem_params_node_t *bl_mem_params;

	if (image_id != BL31_IMAGE_ID)
		return 0;

	bl_mem_params = get_bl_mem_params_node(image_id);

	if (is_ddr_backup_mode() == RCAR_COLD_BOOT)
		goto cold_boot;

	*boot_kind  = RCAR_WARM_BOOT;
	flush_dcache_range(BOOT_KIND_BASE, sizeof(*boot_kind));

	console_flush();
	bl2_plat_flush_bl31_params();

	/* will not return */
	bl2_enter_bl31(&bl_mem_params->ep_info);

cold_boot:
	*boot_kind  = RCAR_COLD_BOOT;
	flush_dcache_range(BOOT_KIND_BASE, sizeof(*boot_kind));

	return 0;
}

int bl2_plat_handle_post_image_load(unsigned int image_id)
{
	static bl2_to_bl31_params_mem_t *params;
	bl_mem_params_node_t *bl_mem_params;

	if (!params) {
		params = (bl2_to_bl31_params_mem_t *) PARAMS_BASE;
		memset((void *)PARAMS_BASE, 0, sizeof(*params));
	}

	bl_mem_params = get_bl_mem_params_node(image_id);

	switch (image_id) {
	case BL31_IMAGE_ID:
		break;
	case BL32_IMAGE_ID:
		memcpy(&params->bl32_ep_info, &bl_mem_params->ep_info,
			sizeof(entry_point_info_t));
		break;
	case BL33_IMAGE_ID:
		memcpy(&params->bl33_ep_info, &bl_mem_params->ep_info,
			sizeof(entry_point_info_t));
		break;
	}

	return 0;
}

meminfo_t *bl2_plat_sec_mem_layout(void)
{
	return &bl2_tzram_layout;
}

void bl2_el3_early_platform_setup(u_register_t arg1, u_register_t arg2,
				  u_register_t arg3, u_register_t arg4)
{
	uint32_t reg, midr, lcs, boot_dev, boot_cpu, sscg, type, rev;
	uint32_t cut, product, product_cut, major, minor;
	int32_t ret;
	const char *str;
	const char *unknown = "unknown";
	const char *cpu_ca57 = "CA57";
	const char *cpu_ca53 = "CA53";
	const char *product_m3n = "M3N";
	const char *product_h3 = "H3";
	const char *product_m3 = "M3";
	const char *product_e3 = "E3";
	const char *lcs_secure = "SE";
	const char *lcs_cm = "CM";
	const char *lcs_dm = "DM";
	const char *lcs_sd = "SD";
	const char *lcs_fa = "FA";
	const char *sscg_off = "PLL1 nonSSCG Clock select";
	const char *sscg_on = "PLL1 SSCG Clock select";
	const char *boot_hyper80 = "HyperFlash(80MHz)";
	const char *boot_qspi40 = "QSPI Flash(40MHz)";
	const char *boot_qspi80 = "QSPI Flash(80MHz)";
	const char *boot_emmc25x1 = "eMMC(25MHz x1)";
	const char *boot_emmc50x8 = "eMMC(50MHz x8)";
#if RCAR_LSI == RCAR_E3
	const char *boot_hyper160 = "HyperFlash(150MHz)";
#else
	const char *boot_hyper160 = "HyperFlash(160MHz)";
#endif

	reg = mmio_read_32(RCAR_MODEMR);
	boot_dev = reg & MODEMR_BOOT_DEV_MASK;
	boot_cpu = reg & MODEMR_BOOT_CPU_MASK;

	bl2_cpg_init();

	if (boot_cpu == MODEMR_BOOT_CPU_CA57 ||
	    boot_cpu == MODEMR_BOOT_CPU_CA53) {
		rcar_pfc_init();
		/* console configuration (platform specific) done in driver */
		console_init(0, 0, 0);
	}

	plat_rcar_gic_driver_init();
	plat_rcar_gic_init();
	rcar_swdt_init();

	/* FIQ interrupts are taken to EL3 */
	write_scr_el3(read_scr_el3() | SCR_FIQ_BIT);

	write_daifclr(DAIF_FIQ_BIT);

	reg = read_midr();
	midr = reg & (MIDR_PN_MASK << MIDR_PN_SHIFT);
	switch (midr) {
	case MIDR_CA57:
		str = cpu_ca57;
		break;
	case MIDR_CA53:
		str = cpu_ca53;
		break;
	default:
		str = unknown;
		break;
	}

	NOTICE("BL2: R-Car Gen3 Initial Program Loader(%s) Rev.%s\n", str,
	       version_of_renesas);

	reg = mmio_read_32(RCAR_PRR);
	product_cut = reg & (RCAR_PRODUCT_MASK | RCAR_CUT_MASK);
	product = reg & RCAR_PRODUCT_MASK;
	cut = reg & RCAR_CUT_MASK;

	switch (product) {
	case RCAR_PRODUCT_H3:
		str = product_h3;
		break;
	case RCAR_PRODUCT_M3:
		str = product_m3;
		break;
	case RCAR_PRODUCT_M3N:
		str = product_m3n;
		break;
	case RCAR_PRODUCT_E3:
		str = product_e3;
		break;
	default:
		str = unknown;
		break;
	}

	if (RCAR_PRODUCT_M3_CUT11 == product_cut) {
		NOTICE("BL2: PRR is R-Car %s Ver.1.1 / Ver.1.2\n", str);
	} else {
		major = (reg & RCAR_MAJOR_MASK) >> RCAR_MAJOR_SHIFT;
		major = major + RCAR_MAJOR_OFFSET;
		minor = reg & RCAR_MINOR_MASK;
		NOTICE("BL2: PRR is R-Car %s Ver.%d.%d\n", str, major, minor);
	}

	if (product == RCAR_PRODUCT_E3) {
		reg = mmio_read_32(RCAR_MODEMR);
		sscg = reg & RCAR_SSCG_MASK;
		str = sscg == RCAR_SSCG_ENABLE ? sscg_on : sscg_off;
		NOTICE("BL2: %s\n", str);
	}

	rcar_get_board_type(&type, &rev);

	switch (type) {
	case BOARD_SALVATOR_X:
	case BOARD_KRIEK:
	case BOARD_STARTER_KIT:
	case BOARD_SALVATOR_XS:
	case BOARD_EBISU:
	case BOARD_STARTER_KIT_PRE:
	case BOARD_EBISU_4D:
		break;
	default:
		type = BOARD_UNKNOWN;
		break;
	}

	if (type == BOARD_UNKNOWN || rev == BOARD_REV_UNKNOWN)
		NOTICE("BL2: Board is %s Rev.---\n", GET_BOARD_NAME(type));
	else {
		NOTICE("BL2: Board is %s Rev.%d.%d\n",
		       GET_BOARD_NAME(type),
		       GET_BOARD_MAJOR(rev), GET_BOARD_MINOR(rev));
	}

#if RCAR_LSI != RCAR_AUTO
	if (product != TARGET_PRODUCT) {
		ERROR("BL2: IPL was been built for the %s.\n", TARGET_NAME);
		ERROR("BL2: Please write the correct IPL to flash memory.\n");
		panic();
	}
#endif
	rcar_avs_init();
	rcar_avs_setting();

	switch (boot_dev) {
	case MODEMR_BOOT_DEV_HYPERFLASH160:
		str = boot_hyper160;
		break;
	case MODEMR_BOOT_DEV_HYPERFLASH80:
		str = boot_hyper80;
		break;
	case MODEMR_BOOT_DEV_QSPI_FLASH40:
		str = boot_qspi40;
		break;
	case MODEMR_BOOT_DEV_QSPI_FLASH80:
		str = boot_qspi80;
		break;
	case MODEMR_BOOT_DEV_EMMC_25X1:
		str = boot_emmc25x1;
		break;
	case MODEMR_BOOT_DEV_EMMC_50X8:
		str = boot_emmc50x8;
		break;
	default:
		str = unknown;
		break;
	}
	NOTICE("BL2: Boot device is %s\n", str);

	rcar_avs_setting();
	reg = rcar_rom_get_lcs(&lcs);
	if (reg) {
		str = unknown;
		goto lcm_state;
	}

	switch (lcs) {
	case LCS_CM:
		str = lcs_cm;
		break;
	case LCS_DM:
		str = lcs_dm;
		break;
	case LCS_SD:
		str = lcs_sd;
		break;
	case LCS_SE:
		str = lcs_secure;
		break;
	case LCS_FA:
		str = lcs_fa;
		break;
	default:
		str = unknown;
		break;
	}

lcm_state:
	NOTICE("BL2: LCM state is %s\n", str);

	rcar_avs_end();
	is_ddr_backup_mode();

	bl2_tzram_layout.total_base = BL31_BASE;
	bl2_tzram_layout.total_size = BL31_LIMIT - BL31_BASE;

	if (product == RCAR_PRODUCT_H3 && cut >= RCAR_CUT_VER30) {
#if (RCAR_DRAM_LPDDR4_MEMCONF == 0)
		NOTICE("BL2: CH0: 0x400000000 - 0x440000000, 1 GiB\n");
		NOTICE("BL2: CH1: 0x500000000 - 0x540000000, 1 GiB\n");
		NOTICE("BL2: CH2: 0x600000000 - 0x640000000, 1 GiB\n");
		NOTICE("BL2: CH3: 0x700000000 - 0x740000000, 1 GiB\n");
#elif (RCAR_DRAM_LPDDR4_MEMCONF == 1) && \
      (RCAR_DRAM_CHANNEL        == 5) && \
      (RCAR_DRAM_SPLIT          == 2)
		NOTICE("BL2: CH0: 0x400000000 - 0x480000000, 2 GiB\n");
		NOTICE("BL2: CH1: 0x500000000 - 0x580000000, 2 GiB\n");
#elif (RCAR_DRAM_LPDDR4_MEMCONF == 1) && (RCAR_DRAM_CHANNEL == 15)
		NOTICE("BL2: CH0: 0x400000000 - 0x480000000, 2 GiB\n");
		NOTICE("BL2: CH1: 0x500000000 - 0x580000000, 2 GiB\n");
		NOTICE("BL2: CH2: 0x600000000 - 0x680000000, 2 GiB\n");
		NOTICE("BL2: CH3: 0x700000000 - 0x780000000, 2 GiB\n");
#endif
	}

	if (product == RCAR_PRODUCT_E3) {
#if (RCAR_DRAM_DDR3L_MEMCONF == 0)
		NOTICE("BL2: 0x400000000 - 0x440000000, 1 GiB\n");
#elif (RCAR_DRAM_DDR3L_MEMCONF == 1)
		NOTICE("BL2: 0x400000000 - 0x480000000, 2 GiB\n");
#endif
	}

	if (boot_cpu == MODEMR_BOOT_CPU_CA57 ||
	    boot_cpu == MODEMR_BOOT_CPU_CA53) {
		ret = rcar_dram_init();
		if (ret) {
			NOTICE("BL2: Failed to DRAM initialize (%d).\n", ret);
			panic();
		}
		rcar_qos_init();
	}

	if (boot_dev == MODEMR_BOOT_DEV_EMMC_25X1 ||
	    boot_dev == MODEMR_BOOT_DEV_EMMC_50X8) {
		if (rcar_emmc_init() != EMMC_SUCCESS) {
			NOTICE("BL2: Failed to eMMC driver initialize.\n");
			panic();
		}
		rcar_emmc_memcard_power(EMMC_POWER_ON);
		if (rcar_emmc_mount() != EMMC_SUCCESS) {
			NOTICE("BL2: Failed to eMMC mount operation.\n");
			panic();
		}
	} else {
		rcar_rpc_init();
		rcar_dma_init();
	}

	reg = mmio_read_32(RST_WDTRSTCR);
	reg &= ~WDTRSTCR_RWDT_RSTMSK;
	reg |= WDTRSTCR_PASSWORD;
	mmio_write_32(RST_WDTRSTCR, reg);

	mmio_write_32(CPG_CPGWPR, CPGWPR_PASSWORD);
	mmio_write_32(CPG_CPGWPCR, CPGWPCR_PASSWORD);

	reg = mmio_read_32(RCAR_PRR);
	if ((reg & RCAR_CPU_MASK_CA57) == RCAR_CPU_HAVE_CA57)
		mmio_write_32(CPG_CA57DBGRCR,
			      DBGCPUPREN | mmio_read_32(CPG_CA57DBGRCR));

	if ((reg & RCAR_CPU_MASK_CA53) == RCAR_CPU_HAVE_CA53)
		mmio_write_32(CPG_CA53DBGRCR,
			      DBGCPUPREN | mmio_read_32(CPG_CA53DBGRCR));

	if (product_cut == RCAR_PRODUCT_H3_CUT10) {
		reg = mmio_read_32(CPG_PLL2CR);
		reg &= ~((uint32_t) 1 << 5);
		mmio_write_32(CPG_PLL2CR, reg);

		reg = mmio_read_32(CPG_PLL4CR);
		reg &= ~((uint32_t) 1 << 5);
		mmio_write_32(CPG_PLL4CR, reg);

		reg = mmio_read_32(CPG_PLL0CR);
		reg &= ~((uint32_t) 1 << 12);
		mmio_write_32(CPG_PLL0CR, reg);
	}
#if (RCAR_LOSSY_ENABLE == 1)
	NOTICE("BL2: Lossy Decomp areas\n");
	bl2_lossy_setting(0, LOSSY_ST_ADDR0, LOSSY_END_ADDR0,
			  LOSSY_FMT0, LOSSY_ENA_DIS0);
	bl2_lossy_setting(1, LOSSY_ST_ADDR1, LOSSY_END_ADDR1,
			  LOSSY_FMT1, LOSSY_ENA_DIS1);
	bl2_lossy_setting(2, LOSSY_ST_ADDR2, LOSSY_END_ADDR2,
			  LOSSY_FMT2, LOSSY_ENA_DIS2);
#endif

	if (boot_dev == MODEMR_BOOT_DEV_EMMC_25X1 ||
	    boot_dev == MODEMR_BOOT_DEV_EMMC_50X8)
		rcar_io_emmc_setup();
	else
		rcar_io_setup();
}

void bl2_el3_plat_arch_setup(void)
{
#if RCAR_BL2_DCACHE == 1
	NOTICE("BL2: D-Cache enable\n");
	rcar_configure_mmu_el3(BL2_BASE,
			       RCAR_SYSRAM_LIMIT - BL2_BASE,
			       BL2_RO_BASE, BL2_RO_LIMIT
#if USE_COHERENT_MEM
			       , BL2_COHERENT_RAM_BASE, BL2_COHERENT_RAM_LIMIT
#endif
	    );
#endif
}

void bl2_platform_setup(void)
{

}
