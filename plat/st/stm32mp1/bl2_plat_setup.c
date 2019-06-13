/*
 * Copyright (c) 2015-2019, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch_helpers.h>
#include <assert.h>
#include <bl_common.h>
#include <boot_api.h>
#include <bsec.h>
#include <debug.h>
#include <delay_timer.h>
#include <desc_image_load.h>
#include <dt-bindings/clock/stm32mp1-clks.h>
#include <dt-bindings/reset/stm32mp1-resets.h>
#include <generic_delay_timer.h>
#include <mmio.h>
#include <optee_utils.h>
#include <platform.h>
#include <platform_def.h>
#include <stm32_console.h>
#include <stm32_gpio.h>
#include <stm32_iwdg.h>
#include <stm32mp_auth.h>
#include <stm32mp_common.h>
#include <stm32mp_dt.h>
#include <stm32mp_pmic.h>
#include <stm32mp_reset.h>
#include <stm32mp1xx_hal_uart.h>
#include <stm32mp1_clk.h>
#include <stm32mp1_context.h>
#include <stm32mp1_dbgmcu.h>
#include <stm32mp1_private.h>
#include <stm32mp1_pwr.h>
#include <stm32mp1_ram.h>
#include <stm32mp1_rcc.h>
#include <stm32mp1_shared_resources.h>
#include <string.h>
#include <xlat_tables_v2.h>

#define PWRLP_TEMPO_5_HSI	5

static struct console_stm32 console;
static enum boot_device_e boot_device = BOOT_DEVICE_BOARD;
static struct auth_ops stm32mp_auth_ops;

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
		if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U)
			INFO("  System reset generated by MCU (MCSYSRST)\n");
		else
			INFO("  Local reset generated by MCU (MCSYSRST)\n");
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

void bl2_platform_setup(void)
{
	int ret;

	if (dt_pmic_status() > 0) {
		initialize_pmic();
#if STM32MP1_DEBUG_ENABLE
		/* Program PMIC to keep debug ON */
		if ((stm32mp1_dbgmcu_boot_debug_info() == 1) &&
		    (stm32mp1_dbgmcu_is_debug_on() != 0)) {
			VERBOSE("Program PMIC to keep debug ON\n");
			if (pmic_keep_debug_unit() != 0) {
				ERROR("PMIC not properly set for debug\n");
			}
		}
#endif
	}

	ret = stm32mp1_ddr_probe();
	if (ret < 0) {
		ERROR("Invalid DDR init: error %d\n", ret);
		panic();
	}

#ifdef AARCH32_SP_OPTEE
	INFO("BL2 runs OP-TEE setup\n");
	/* Initialize tzc400 after DDR initialization */
	stm32mp1_security_setup();
#else
	INFO("BL2 runs SP_MIN setup\n");
#endif
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
			BL_RO_DATA_LIMIT - BL_RO_DATA_BASE,
			MT_RO_DATA | MT_SECURE);
#endif

#ifdef AARCH32_SP_OPTEE
	/* OP-TEE image needs post load processing: keep RAM read/write */
	mmap_add_region(STM32MP_DDR_BASE + dt_get_ddr_size() -
			STM32MP_DDR_S_SIZE,
			STM32MP_DDR_BASE + dt_get_ddr_size() -
			STM32MP_DDR_S_SIZE,
			STM32MP_DDR_S_SIZE,
			MT_MEMORY | MT_RW | MT_SECURE);

	mmap_add_region(STM32MP_OPTEE_BASE, STM32MP_OPTEE_BASE,
			STM32MP_OPTEE_SIZE,
			MT_MEMORY | MT_RW | MT_SECURE);
#else
	/* Prevent corruption of preloaded BL32 */
	mmap_add_region(BL32_BASE, BL32_BASE,
			BL32_LIMIT - BL32_BASE,
			MT_MEMORY | MT_RO | MT_SECURE);

#endif
	/* Map non secure DDR for BL33 load and DDR training area restore */
	mmap_add_region(STM32MP_DDR_BASE,
			STM32MP_DDR_BASE,
			STM32MP_DDR_MAX_SIZE,
			MT_MEMORY | MT_RW | MT_NS);

	/* Prevent corruption of preloaded Device Tree */
	mmap_add_region(DTB_BASE, DTB_BASE,
			DTB_LIMIT - DTB_BASE,
			MT_MEMORY | MT_RO | MT_SECURE);

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

	generic_delay_timer_init();

#ifdef STM32MP_USB
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

	if (stm32mp1_clk_init() < 0) {
		panic();
	}

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

	stm32mp_reset_assert((uint32_t)dt_uart_info.reset);
	udelay(2);
	stm32mp_reset_deassert((uint32_t)dt_uart_info.reset);
	mdelay(1);

	clk_rate = stm32mp_clk_get_rate((unsigned long)dt_uart_info.clock);

	if (console_stm32_register(dt_uart_info.base, clk_rate,
				   STM32MP_UART_BAUDRATE, &console) == 0) {
		panic();
	}

	stm32mp_print_cpuinfo();
	board_model = dt_get_board_model();
	if (board_model != NULL) {
		NOTICE("Model: %s\n", board_model);
	}
	stm32mp_print_boardinfo();

	if (boot_context->auth_status != BOOT_API_CTX_AUTH_NO) {
		NOTICE("%s\n", (boot_context->auth_status ==
				BOOT_API_CTX_AUTH_FAILED) ?
		       "Boot authentication Failed" :
		       "Boot authentication Success");
	}

skip_console_init:

	/* Initialize IWDG Status, no startup */
	if (stm32_iwdg_init() < 0) {
		panic();
	}

	/* Reload watchdog */
	stm32_iwdg_refresh(IWDG2_INST);

	result = stm32mp1_dbgmcu_freeze_iwdg2();
	if (result != 0) {
		INFO("IWDG2 freeze error : %i\n", result);
	}

	if (stm32_save_boot_interface(boot_context->boot_interface_selected,
				      boot_context->boot_interface_instance) !=
	    0) {
		ERROR("Cannot save boot interface\n");
	}

	stm32mp_auth_ops.check_key =
		boot_context->p_bootrom_ext_service_ecdsa_check_key;
	stm32mp_auth_ops.verify_signature =
		boot_context->p_bootrom_ext_service_ecdsa_verify_signature;

	stm32mp_init_auth(&stm32mp_auth_ops);

	stm32mp1_arch_security_setup();

	print_reset_reason();

	stm32mp_io_setup();

	stm32mp1_syscfg_init();
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
				      STM32MP_DDR_S_SIZE;
		unpaged->image_max_size = STM32MP_DDR_S_SIZE;
	}
	paged->image_base = STM32MP_DDR_BASE + dt_get_ddr_size() -
			    STM32MP_DDR_S_SIZE;
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

	switch (image_id) {
	case BL32_IMAGE_ID:
#if defined(AARCH32_SP_OPTEE)
		pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
		assert(pager_mem_params);

		paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
		assert(paged_mem_params);

		bl_mem_params->ep_info.pc =
					bl_mem_params->image_info.image_base;

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
		assert(bl32_mem_params);
		bl32_mem_params->ep_info.lr_svc = bl_mem_params->ep_info.pc;
#else
		/* BL33 expects to receive : TBD */
		bl_mem_params->ep_info.args.arg0 = 0;
		bl_mem_params->ep_info.spsr =
			SPSR_MODE32(MODE32_svc, SPSR_T_ARM, SPSR_E_LITTLE,
				    DISABLE_ALL_EXCEPTIONS);
#endif
		flush_dcache_range(bl_mem_params->image_info.image_base,
				   bl_mem_params->image_info.image_max_size);
		break;

#ifdef AARCH32_SP_OPTEE
	case BL32_EXTRA1_IMAGE_ID:
	case BL32_EXTRA2_IMAGE_ID:
		clean_dcache_range(bl_mem_params->image_info.image_base,
				   bl_mem_params->image_info.image_max_size);
		break;
#endif

	default:
		err = -1;
		break;
	}

	return err;
}

