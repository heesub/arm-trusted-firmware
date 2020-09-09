/*
 * Copyright (c) 2015-2020, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <string.h>

#include <platform_def.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <common/desc_image_load.h>
#include <drivers/delay_timer.h>
#include <drivers/generic_delay_timer.h>
#include <drivers/st/bsec.h>
#include <drivers/st/stm32_console.h>
#include <drivers/st/stm32_iwdg.h>
#include <drivers/st/stm32mp_clkfunc.h>
#include <drivers/st/stm32mp_pmic.h>
#include <drivers/st/stm32mp_reset.h>
#include <drivers/st/stm32mp1_clk.h>
#include <drivers/st/stm32mp1_pwr.h>
#include <drivers/st/stm32mp1_ram.h>
#if STM32MP_UART_PROGRAMMER
#include <drivers/st/stm32mp1xx_hal_uart.h>
#endif
#include <drivers/st/stpmic1.h>
#include <lib/mmio.h>
#include <lib/optee_utils.h>
#include <lib/ssp_lib.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/platform.h>

#include <boot_api.h>
#include <stm32mp1_context.h>
#include <stm32mp1_dbgmcu.h>

#define PWRLP_TEMPO_5_HSI	5

#define TIMEOUT_US_1MS		U(1000)

#if !STM32MP_SSP
static const char debug_msg[626] = {
	"***************************************************\n"
	"** NOTICE   NOTICE   NOTICE   NOTICE   NOTICE    **\n"
	"**                                               **\n"
	"** DEBUG ACCESS PORT IS OPEN!                    **\n"
	"** This boot image is only for debugging purpose **\n"
	"** and is unsafe for production use.             **\n"
	"**                                               **\n"
	"** If you see this message and you are not       **\n"
	"** debugging report this immediately to your     **\n"
	"** vendor!                                       **\n"
	"**                                               **\n"
	"***************************************************\n"
};
#endif

static struct console_stm32 console;
static enum boot_device_e boot_device = BOOT_DEVICE_BOARD;
static bool wakeup_standby;

static void print_reset_reason(void)
{
	uint32_t rstsr = mmio_read_32(stm32mp_rcc_base() + RCC_MP_RSTSCLRR);

	if (rstsr == 0U) {
		WARN("Reset reason unknown\n");
		return;
	}

	INFO("Reset reason (0x%x):\n", rstsr);

	if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) == 0U) {
		if ((rstsr & RCC_MP_RSTSCLRR_STDBYRSTF) != 0U) {
			INFO("System exits from STANDBY\n");
			return;
		}

		if ((rstsr & RCC_MP_RSTSCLRR_CSTDBYRSTF) != 0U) {
			INFO("MPU exits from CSTANDBY\n");
			return;
		}
	}

	if ((rstsr & RCC_MP_RSTSCLRR_PORRSTF) != 0U) {
		INFO("  Power-on Reset (rst_por)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_BORRSTF) != 0U) {
		INFO("  Brownout Reset (rst_bor)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MCSYSRSTF) != 0U) {
		if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U) {
			INFO("  System reset generated by MCU (MCSYSRST)\n");
		} else {
			INFO("  Local reset generated by MCU (MCSYSRST)\n");
		}
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPSYSRSTF) != 0U) {
		INFO("  System reset generated by MPU (MPSYSRST)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_HCSSRSTF) != 0U) {
		INFO("  Reset due to a clock failure on HSE\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_IWDG1RSTF) != 0U) {
		INFO("  IWDG1 Reset (rst_iwdg1)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_IWDG2RSTF) != 0U) {
		INFO("  IWDG2 Reset (rst_iwdg2)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPUP0RSTF) != 0U) {
		INFO("  MPU Processor 0 Reset\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPUP1RSTF) != 0U) {
		INFO("  MPU Processor 1 Reset\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U) {
		INFO("  Pad Reset from NRST\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_VCORERSTF) != 0U) {
		INFO("  Reset due to a failure of VDD_CORE\n");
		return;
	}

	ERROR("  Unidentified reset reason\n");
}

enum boot_device_e get_boot_device(void)
{
	return boot_device;
}

void bl2_el3_early_platform_setup(u_register_t arg0,
				  u_register_t arg1 __unused,
				  u_register_t arg2 __unused,
				  u_register_t arg3 __unused)
{
	stm32mp_save_boot_ctx_address(arg0);
}

#if STM32MP_SSP
void bl2_platform_setup(void)
{
}
#else
void bl2_platform_setup(void)
{
	int ret;

	/*
	 * Map DDR non cacheable during its initialisation to avoid
	 * speculative loads before accesses are fully setup.
	 */
	ret = mmap_add_dynamic_region(STM32MP_DDR_BASE, STM32MP_DDR_BASE,
				      STM32MP_DDR_MAX_SIZE,
				      MT_NON_CACHEABLE | MT_RW | MT_NS);
	assert(ret == 0);

	ret = stm32mp1_ddr_probe();
	if (ret < 0) {
		ERROR("Invalid DDR init: error %d\n", ret);
		panic();
	}

	ret = mmap_remove_dynamic_region(STM32MP_DDR_BASE,
					 STM32MP_DDR_MAX_SIZE);
	assert(ret == 0);

#ifdef AARCH32_SP_OPTEE
	INFO("BL2 runs OP-TEE setup\n");

	/* Map non secure DDR for BL33 load, now with cacheable attribute */
	ret = mmap_add_dynamic_region(STM32MP_DDR_BASE, STM32MP_DDR_BASE,
				      dt_get_ddr_size()  - STM32MP_DDR_S_SIZE -
				      STM32MP_DDR_SHMEM_SIZE,
				      MT_MEMORY | MT_RW | MT_NS);
	assert(ret == 0);

	ret = mmap_add_dynamic_region(STM32MP_DDR_BASE + dt_get_ddr_size() -
				      STM32MP_DDR_S_SIZE -
				      STM32MP_DDR_SHMEM_SIZE,
				      STM32MP_DDR_BASE + dt_get_ddr_size() -
				      STM32MP_DDR_S_SIZE -
				      STM32MP_DDR_SHMEM_SIZE,
				      STM32MP_DDR_S_SIZE,
				      MT_MEMORY | MT_RW | MT_SECURE);
	assert(ret == 0);

	/* Initialize tzc400 after DDR initialization */
	stm32mp1_security_setup();
#else
	INFO("BL2 runs SP_MIN setup\n");

	/* Map non secure DDR for BL33 load, now with cacheable attribute */
	ret = mmap_add_dynamic_region(STM32MP_DDR_BASE, STM32MP_DDR_BASE,
				      dt_get_ddr_size(),
				      MT_MEMORY | MT_RW | MT_NS);
	assert(ret == 0);
#endif

	if ((dt_pmic_status() > 0) && (!wakeup_standby)) {
		configure_pmic();
	}
}
#endif /* STM32MP_SSP */

static void update_monotonic_counter(void)
{
	uint32_t version;
	uint32_t otp;

	CASSERT(STM32_TF_VERSION <= MAX_MONOTONIC_VALUE,
		assert_stm32mp1_monotonic_counter_reach_max);

	/* Check if monotonic counter needs to be incremented */
	if (stm32_get_otp_index(MONOTONIC_OTP, &otp, NULL) != 0) {
		panic();
	}

	if (stm32_get_otp_value(MONOTONIC_OTP, &version) != 0) {
		panic();
	}

	if ((version + 1U) < BIT(STM32_TF_VERSION)) {
		uint32_t result;

		/* Need to increment the monotonic counter. */
		version = BIT(STM32_TF_VERSION) - 1U;

		result = bsec_program_otp(version, otp);
		if (result != BSEC_OK) {
			ERROR("BSEC: MONOTONIC_OTP program Error %i\n",
			      result);
			panic();
		}
		INFO("Monotonic counter has been incremented (value 0x%x)\n",
		     version);
	}
}

static void initialize_clock(void)
{
	uint32_t voltage_mv = 0U;
	uint32_t freq_khz = 0U;
	int ret = 0;

#if !STM32MP_SSP
	if (wakeup_standby) {
		ret = stm32_get_pll1_settings_from_context();
	}
#endif

	/*
	 * If no pre-defined PLL1 settings in DT, find the highest frequency
	 * in the OPP table (in DT, compatible with plaform capabilities, or
	 * in structure restored in RAM), and set related CPU supply voltage.
	 * If PLL1 settings found in DT, we consider CPU supply voltage in DT
	 * is consistent with it.
	 */
	if ((ret == 0) && !fdt_is_pll1_predefined()) {
		if (wakeup_standby) {
			ret = stm32mp1_clk_get_maxfreq_opp(&freq_khz,
							   &voltage_mv);
		} else {
			ret = dt_get_max_opp_freqvolt(&freq_khz, &voltage_mv);
		}

		if (ret != 0) {
			panic();
		}

		if (dt_pmic_status() > 0) {
			int read_voltage;
			const char *name;

			name = stm32mp_get_cpu_supply_name();
			if (name == NULL) {
				panic();
			}

			read_voltage = stpmic1_regulator_voltage_get(name);
			if (read_voltage < 0) {
				panic();
			}

			if (voltage_mv != (uint32_t)read_voltage) {
				if (stpmic1_regulator_voltage_set(name,
						(uint16_t)voltage_mv) != 0) {
					panic();
				}
			}
		}
	}

	if (stm32mp1_clk_init(freq_khz) < 0) {
		panic();
	}
}

static void reset_uart(uint32_t reset)
{
	if (stm32mp_reset_assert_to(reset, TIMEOUT_US_1MS)) {
		panic();
	}

	udelay(2);

	if (stm32mp_reset_deassert_to(reset, TIMEOUT_US_1MS)) {
		panic();
	}

	mdelay(1);
}

void bl2_el3_plat_arch_setup(void)
{
	int32_t result;
	struct dt_node_info dt_uart_info;
	const char *board_model;
	boot_api_context_t *boot_context =
		(boot_api_context_t *)stm32mp_get_boot_ctx_address();
	uint32_t clk_rate;
	uintptr_t pwr_base;
	uintptr_t rcc_base;
	uint32_t bkpr_core1_magic =
		tamp_bkpr(BOOT_API_CORE1_MAGIC_NUMBER_TAMP_BCK_REG_IDX);
	uint32_t bkpr_core1_addr =
		tamp_bkpr(BOOT_API_CORE1_BRANCH_ADDRESS_TAMP_BCK_REG_IDX);

	mmap_add_region(BL_CODE_BASE, BL_CODE_BASE,
			BL_CODE_END - BL_CODE_BASE,
			MT_CODE | MT_SECURE);

#if SEPARATE_CODE_AND_RODATA
	mmap_add_region(BL_RO_DATA_BASE, BL_RO_DATA_BASE,
			BL_RO_DATA_END - BL_RO_DATA_BASE,
			MT_RO_DATA | MT_SECURE);
#endif

#if !STM32MP_SSP
#ifdef AARCH32_SP_OPTEE
	mmap_add_region(STM32MP_OPTEE_BASE, STM32MP_OPTEE_BASE,
			STM32MP_OPTEE_SIZE,
			MT_MEMORY | MT_RW | MT_SECURE);
#else
	/* Prevent corruption of preloaded BL32 */
	mmap_add_region(BL32_BASE, BL32_BASE,
			BL32_LIMIT - BL32_BASE,
			MT_RO_DATA | MT_SECURE);
#endif
#endif
	/* Prevent corruption of preloaded Device Tree */
	mmap_add_region(DTB_BASE, DTB_BASE,
			DTB_LIMIT - DTB_BASE,
			MT_RO_DATA | MT_SECURE);

	configure_mmu();

	if (dt_open_and_check() < 0) {
		panic();
	}

	pwr_base = stm32mp_pwr_base();
	rcc_base = stm32mp_rcc_base();

	/* Clear Stop Request bits to correctly manage low-power exit */
	mmio_write_32(rcc_base + RCC_MP_SREQCLRR,
		      (uint32_t)(RCC_MP_SREQCLRR_STPREQ_P0 |
				 RCC_MP_SREQCLRR_STPREQ_P1));

	/*
	 * Disable the backup domain write protection.
	 * The protection is enable at each reset by hardware
	 * and must be disabled by software.
	 */
	mmio_setbits_32(pwr_base + PWR_CR1, PWR_CR1_DBP);

	while ((mmio_read_32(pwr_base + PWR_CR1) & PWR_CR1_DBP) == 0U) {
		;
	}

	/*
	 * Configure Standby mode available for MCU by default
	 * and allow to switch in standby SoC in all case
	 */
	mmio_setbits_32(pwr_base + PWR_MCUCR, PWR_MCUCR_PDDS);

	if (bsec_probe() != 0) {
		panic();
	}

	/* Reset backup domain on cold boot cases */
	if ((mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_RTCSRC_MASK) == 0U) {
		mmio_setbits_32(rcc_base + RCC_BDCR, RCC_BDCR_VSWRST);

		while ((mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_VSWRST) ==
		       0U) {
			;
		}

		mmio_clrbits_32(rcc_base + RCC_BDCR, RCC_BDCR_VSWRST);
	}

	/* Wait 5 HSI periods before re-enabling PLLs after STOP modes */
	mmio_clrsetbits_32(rcc_base + RCC_PWRLPDLYCR,
			   RCC_PWRLPDLYCR_PWRLP_DLY_MASK,
			   PWRLP_TEMPO_5_HSI);

	/* Disable retention and backup RAM content after standby */
	mmio_clrbits_32(pwr_base + PWR_CR2, PWR_CR2_BREN | PWR_CR2_RREN);

	/* Disable MCKPROT */
	mmio_clrbits_32(rcc_base + RCC_TZCR, RCC_TZCR_MCKPROT);

	/* Enable BKP Register protection */
	mmio_write_32(TAMP_SMCR,
		      TAMP_BKP_SEC_NUMBER << TAMP_BKP_SEC_WDPROT_SHIFT |
		      TAMP_BKP_SEC_NUMBER << TAMP_BKP_SEC_RWDPROT_SHIFT);

	if ((boot_context->boot_action !=
	     BOOT_API_CTX_BOOT_ACTION_WAKEUP_CSTANDBY) &&
	    (boot_context->boot_action !=
	     BOOT_API_CTX_BOOT_ACTION_WAKEUP_STANDBY)) {
		mmio_write_32(bkpr_core1_addr, 0);
		mmio_write_32(bkpr_core1_magic, 0);
	}

	wakeup_standby = (mmio_read_32(bkpr_core1_addr) != 0U);

	generic_delay_timer_init();

#if STM32MP_USB_PROGRAMMER
	if (boot_context->boot_interface_selected ==
	    BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_USB) {
		boot_device = BOOT_DEVICE_USB;
	}
#endif

#if STM32MP_UART_PROGRAMMER
	/* Disable programmer UART before changing clock tree */
	if (boot_context->boot_interface_selected ==
	    BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_UART) {
		uintptr_t uart_prog_addr =
			get_uart_address(boot_context->boot_interface_instance);

		((USART_TypeDef *)uart_prog_addr)->CR1 &= ~USART_CR1_UE;
	}
#endif

	if (stm32mp1_clk_probe() < 0) {
		panic();
	}

	if (dt_pmic_status() > 0) {
		initialize_pmic();
	}

	initialize_clock();

	result = dt_get_stdout_uart_info(&dt_uart_info);

	if ((result <= 0) ||
	    (dt_uart_info.status == 0U) ||
#if STM32MP_UART_PROGRAMMER
	    ((boot_context->boot_interface_selected ==
	      BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_UART) &&
	     (get_uart_address(boot_context->boot_interface_instance) ==
	      dt_uart_info.base)) ||
#endif
	    (dt_uart_info.clock < 0) ||
	    (dt_uart_info.reset < 0)) {
		goto skip_console_init;
	}

	if (dt_set_stdout_pinctrl() != 0) {
		goto skip_console_init;
	}

	if (dt_uart_info.status == DT_DISABLED) {
		panic();
	} else if (dt_uart_info.status == DT_SECURE) {
		stm32mp_register_secure_periph_iomem(dt_uart_info.base);
	} else {
		stm32mp_register_non_secure_periph_iomem(dt_uart_info.base);
	}

	stm32mp_clk_enable((unsigned long)dt_uart_info.clock);

	reset_uart((uint32_t)dt_uart_info.reset);

	clk_rate = stm32mp_clk_get_rate((unsigned long)dt_uart_info.clock);

	if (console_stm32_register(dt_uart_info.base, clk_rate,
				   STM32MP_UART_BAUDRATE, &console) == 0) {
		panic();
	}

	console_set_scope(&console.console, CONSOLE_FLAG_BOOT |
			  CONSOLE_FLAG_CRASH | CONSOLE_FLAG_TRANSLATE_CRLF);

#if STM32MP_SSP
	if (boot_context->p_ssp_config->ssp_cmd !=
	    BOOT_API_CTX_SSP_CMD_PROV_SECRET_ACK) {
		stm32mp_print_cpuinfo();
		if (!stm32mp_supports_ssp()) {
			ERROR("Chip doesn't support SSP\n");
			panic();
		}
	}
#else
	stm32mp_print_cpuinfo();
#endif

	board_model = dt_get_board_model();
	if (board_model != NULL) {
		NOTICE("Model: %s\n", board_model);
	}

	stm32mp_print_boardinfo();

#if TRUSTED_BOARD_BOOT
	if (boot_context->auth_status != BOOT_API_CTX_AUTH_NO) {
		NOTICE("Bootrom authentication %s\n",
		       (boot_context->auth_status == BOOT_API_CTX_AUTH_FAILED) ?
		       "failed" : "succeeded");
	}
#endif

skip_console_init:
#if !TRUSTED_BOARD_BOOT
	if (stm32mp_is_closed_device()) {
		/* Closed chip required authentication */
		ERROR("Secured chip must enabled TRUSTED_BOARD_BOOT\n");
		panic();
	}
#endif

	stm32mp1_syscfg_init();

	if (stm32_iwdg_init() < 0) {
		panic();
	}

	stm32_iwdg_refresh();

#if !STM32MP_SSP
	if (bsec_read_debug_conf() != 0U) {
		result = stm32mp1_dbgmcu_freeze_iwdg2();
		if (result != 0) {
			INFO("IWDG2 freeze error : %i\n", result);
		}

		if (stm32mp_is_closed_device()) {
			NOTICE("\n%s", debug_msg);
		}
	}

	if (stm32_save_boot_interface(boot_context->boot_interface_selected,
				      boot_context->boot_interface_instance) !=
	    0) {
		ERROR("Cannot save boot interface\n");
	}

	stm32mp1_arch_security_setup();
#endif

	print_reset_reason();

	update_monotonic_counter();

	if (dt_pmic_status() > 0) {
		initialize_pmic();
		print_pmic_info_and_debug();
	}

#if STM32MP_SSP
	if (boot_context->p_ssp_config->ssp_cmd !=
	    BOOT_API_CTX_SSP_CMD_PROV_SECRET_ACK) {
		stm32mp_io_setup();
	}

	ssp_start(boot_context);
#else
	if (!wakeup_standby) {
		stm32mp_io_setup();
	}
#endif
}

#if defined(AARCH32_SP_OPTEE)
static void set_mem_params_info(entry_point_info_t *ep_info,
				image_info_t *unpaged, image_info_t *paged)
{
	uintptr_t bl32_ep = 0;

	/* Use the default dram setup if no valid ep found */
	if (get_optee_header_ep(ep_info, &bl32_ep) &&
	    (bl32_ep >= STM32MP_OPTEE_BASE) &&
	    (bl32_ep < (STM32MP_OPTEE_BASE + STM32MP_OPTEE_SIZE))) {
		assert((STM32MP_OPTEE_BASE >= BL2_LIMIT) ||
		       ((STM32MP_OPTEE_BASE + STM32MP_OPTEE_SIZE) <= BL2_BASE));

		unpaged->image_base = STM32MP_OPTEE_BASE;
		unpaged->image_max_size = STM32MP_OPTEE_SIZE;
	} else {
		unpaged->image_base = STM32MP_DDR_BASE + dt_get_ddr_size() -
				      STM32MP_DDR_S_SIZE -
				      STM32MP_DDR_SHMEM_SIZE;
		unpaged->image_max_size = STM32MP_DDR_S_SIZE;
	}
	paged->image_base = STM32MP_DDR_BASE + dt_get_ddr_size() -
			    STM32MP_DDR_S_SIZE - STM32MP_DDR_SHMEM_SIZE;
	paged->image_max_size = STM32MP_DDR_S_SIZE;
}
#endif

/*******************************************************************************
 * This function can be used by the platforms to update/use image
 * information for given `image_id`.
 ******************************************************************************/
int bl2_plat_handle_post_image_load(unsigned int image_id)
{
	int err = 0;
	bl_mem_params_node_t *bl_mem_params = get_bl_mem_params_node(image_id);
#if defined(AARCH32_SP_OPTEE)
	bl_mem_params_node_t *bl32_mem_params;
	bl_mem_params_node_t *pager_mem_params;
	bl_mem_params_node_t *paged_mem_params;
#endif

	assert(bl_mem_params != NULL);

#if TRUSTED_BOARD_BOOT
	/* Clean header to avoid loaded header reused */
	stm32mp_delete_loaded_header();
#endif

	switch (image_id) {
	case BL32_IMAGE_ID:
#if defined(AARCH32_SP_OPTEE)
		bl_mem_params->ep_info.pc =
					bl_mem_params->image_info.image_base;

		pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
		assert(pager_mem_params != NULL);

		paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
		assert(paged_mem_params != NULL);

		set_mem_params_info(&bl_mem_params->ep_info,
				    &pager_mem_params->image_info,
				    &paged_mem_params->image_info);

		err = parse_optee_header(&bl_mem_params->ep_info,
					 &pager_mem_params->image_info,
					 &paged_mem_params->image_info);
		if (err) {
			ERROR("OPTEE header parse error.\n");
			panic();
		}

		/* Set optee boot info from parsed header data */
		bl_mem_params->ep_info.pc =
				pager_mem_params->image_info.image_base;
		bl_mem_params->ep_info.args.arg0 =
				paged_mem_params->image_info.image_base;
		bl_mem_params->ep_info.args.arg1 = 0; /* Unused */
		bl_mem_params->ep_info.args.arg2 = 0; /* No DT supported */
#endif
		break;

	case BL33_IMAGE_ID:
#ifdef AARCH32_SP_OPTEE
		bl32_mem_params = get_bl_mem_params_node(BL32_IMAGE_ID);
		assert(bl32_mem_params != NULL);
		bl32_mem_params->ep_info.lr_svc = bl_mem_params->ep_info.pc;
#endif

		flush_dcache_range(bl_mem_params->image_info.image_base,
				   bl_mem_params->image_info.image_max_size);
		break;

	default:
		/* Do nothing in default case */
		break;
	}

	return err;
}
