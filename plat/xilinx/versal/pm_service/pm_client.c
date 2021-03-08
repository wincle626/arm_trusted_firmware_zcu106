/*
 * Copyright (c) 2018 - 2019, Xilinx, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * APU specific definition of processors in the subsystem as well as functions
 * for getting information about and changing state of the APU.
 */

#include <assert.h>
#include <bakery_lock.h>
#include <gic_common.h>
#include <gicv3.h>
#include <mmio.h>
#include <plat_ipi.h>
#include <platform.h>
#include <platform_def.h>
#include <utils.h>
#include <versal_def.h>
#include "pm_api_sys.h"
#include "pm_client.h"

#define UNDEFINED_CPUID		(~0)
#define IRQ_MAX		74
#define NUM_GICD_ISENABLER	((IRQ_MAX >> 5) + 1)

DEFINE_BAKERY_LOCK(pm_client_secure_lock);

static const struct pm_ipi apu_ipi = {
	.local_ipi_id = IPI_ID_APU,
	.remote_ipi_id = IPI_ID_PMC,
	.buffer_base = IPI_BUFFER_APU_BASE,
};

/* Order in pm_procs_all array must match cpu ids */
static const struct pm_proc pm_procs_all[] = {
	{
		.node_id = XPM_DEVID_ACPU_0,
		.ipi = &apu_ipi,
		.pwrdn_mask = APU_0_PWRCTL_CPUPWRDWNREQ_MASK,
	},
	{
		.node_id = XPM_DEVID_ACPU_1,
		.ipi = &apu_ipi,
		.pwrdn_mask = APU_1_PWRCTL_CPUPWRDWNREQ_MASK,
	}
};

const struct pm_proc *primary_proc = &pm_procs_all[0];

/* Interrupt to PM node index map */
static enum pm_device_node_idx irq_node_map[IRQ_MAX + 1] = {
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 3 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 7 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 11 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_GPIO, /* 13 */
	XPM_NODEIDX_DEV_I2C_0,
	XPM_NODEIDX_DEV_I2C_1, /* 15 */
	XPM_NODEIDX_DEV_SPI_0,
	XPM_NODEIDX_DEV_SPI_1, /* 17 */
	XPM_NODEIDX_DEV_UART_0,
	XPM_NODEIDX_DEV_UART_1, /* 19 */
	XPM_NODEIDX_DEV_CAN_FD_0,
	XPM_NODEIDX_DEV_CAN_FD_1, /* 21 */
	XPM_NODEIDX_DEV_USB_0,
	XPM_NODEIDX_DEV_USB_0,
	XPM_NODEIDX_DEV_USB_0,
	XPM_NODEIDX_DEV_USB_0,
	XPM_NODEIDX_DEV_USB_0, /* 26 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 31 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 36 */
	XPM_NODEIDX_DEV_TTC_0,
	XPM_NODEIDX_DEV_TTC_0,
	XPM_NODEIDX_DEV_TTC_0, /* 39 */
	XPM_NODEIDX_DEV_TTC_1,
	XPM_NODEIDX_DEV_TTC_1,
	XPM_NODEIDX_DEV_TTC_1, /* 42 */
	XPM_NODEIDX_DEV_TTC_2,
	XPM_NODEIDX_DEV_TTC_2,
	XPM_NODEIDX_DEV_TTC_2, /* 45 */
	XPM_NODEIDX_DEV_TTC_3,
	XPM_NODEIDX_DEV_TTC_3,
	XPM_NODEIDX_DEV_TTC_3, /* 48 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 52 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 55 */
	XPM_NODEIDX_DEV_GEM_0,
	XPM_NODEIDX_DEV_GEM_0, /* 57 */
	XPM_NODEIDX_DEV_GEM_1,
	XPM_NODEIDX_DEV_GEM_1, /* 59 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 63 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 67 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN, /* 71 */
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_MIN,
	XPM_NODEIDX_DEV_USB_0, /* 74 */
};

/**
 * irq_to_pm_node_idx - Get PM node index corresponding to the interrupt number
 * @irq:	Interrupt number
 *
 * Return:	PM node index corresponding to the specified interrupt
 */
static enum pm_device_node_idx irq_to_pm_node_idx(unsigned int irq)
{
	assert(irq <= IRQ_MAX);
	return irq_node_map[irq];
}

/**
 * pm_client_set_wakeup_sources - Set all devices with enabled interrupts as
 *				  wake sources in the LibPM.
 */
static void pm_client_set_wakeup_sources(void)
{
	uint32_t reg_num;
	uint32_t device_id;
	uint8_t pm_wakeup_nodes_set[XPM_NODEIDX_DEV_MAX];
	uintptr_t isenabler1 = PLAT_VERSAL_GICD_BASE + GICD_ISENABLER + 4;

	zeromem(&pm_wakeup_nodes_set, sizeof(pm_wakeup_nodes_set));

	for (reg_num = 0; reg_num < NUM_GICD_ISENABLER; reg_num++) {
		uint32_t base_irq = reg_num << ISENABLER_SHIFT;
		uint32_t reg = mmio_read_32(isenabler1 + (reg_num << 2));

		if (!reg)
			continue;

		while (reg) {
			enum pm_device_node_idx node_idx;
			uint32_t idx, ret, irq, lowest_set = reg & (-reg);

			idx = __builtin_ctz(lowest_set);
			irq = base_irq + idx;

			if (irq > IRQ_MAX)
				break;

			node_idx = irq_to_pm_node_idx(irq);
			reg &= ~lowest_set;

			if ((node_idx != XPM_NODEIDX_DEV_MIN) &&
			    (!pm_wakeup_nodes_set[node_idx])) {
				/* Get device ID from node index */
				device_id = PERIPH_DEVID(node_idx);
				ret = pm_set_wakeup_source(XPM_DEVID_ACPU_0,
							   device_id, 1);
				pm_wakeup_nodes_set[node_idx] = !ret;
			}
		}
	}
}

/**
 * pm_client_suspend() - Client-specific suspend actions
 *
 * This function should contain any PU-specific actions
 * required prior to sending suspend request to PMU
 * Actions taken depend on the state system is suspending to.
 */
void pm_client_suspend(const struct pm_proc *proc, unsigned int state)
{
	bakery_lock_get(&pm_client_secure_lock);

	if (state == PM_STATE_SUSPEND_TO_RAM)
		pm_client_set_wakeup_sources();

	/* Set powerdown request */
	mmio_write_32(FPD_APU_PWRCTL, mmio_read_32(FPD_APU_PWRCTL) |
		      proc->pwrdn_mask);

	bakery_lock_release(&pm_client_secure_lock);
}

/**
 * pm_client_abort_suspend() - Client-specific abort-suspend actions
 *
 * This function should contain any PU-specific actions
 * required for aborting a prior suspend request
 */
void pm_client_abort_suspend(void)
{
	/* Enable interrupts at processor level (for current cpu) */
	gicv3_cpuif_enable(plat_my_core_pos());

	bakery_lock_get(&pm_client_secure_lock);

	/* Clear powerdown request */
	mmio_write_32(FPD_APU_PWRCTL, mmio_read_32(FPD_APU_PWRCTL) &
		      ~primary_proc->pwrdn_mask);

	bakery_lock_release(&pm_client_secure_lock);
}

/**
 * pm_get_cpuid() - get the local cpu ID for a global node ID
 * @nid:	node id of the processor
 *
 * Return: the cpu ID (starting from 0) for the subsystem
 */
static unsigned int pm_get_cpuid(uint32_t nid)
{
	for (size_t i = 0; i < ARRAY_SIZE(pm_procs_all); i++) {
		if (pm_procs_all[i].node_id == nid)
			return i;
	}
	return UNDEFINED_CPUID;
}

/**
 * pm_client_wakeup() - Client-specific wakeup actions
 *
 * This function should contain any PU-specific actions
 * required for waking up another APU core
 */
void pm_client_wakeup(const struct pm_proc *proc)
{
	unsigned int cpuid = pm_get_cpuid(proc->node_id);

	if (cpuid == UNDEFINED_CPUID)
		return;

	bakery_lock_get(&pm_client_secure_lock);

	/* clear powerdown bit for affected cpu */
	uint32_t val = mmio_read_32(FPD_APU_PWRCTL);
	val &= ~(proc->pwrdn_mask);
	mmio_write_32(FPD_APU_PWRCTL, val);

	bakery_lock_release(&pm_client_secure_lock);
}

/**
 * pm_get_proc() - returns pointer to the proc structure
 * @cpuid:	id of the cpu whose proc struct pointer should be returned
 *
 * Return: pointer to a proc structure if proc is found, otherwise NULL
 */
const struct pm_proc *pm_get_proc(unsigned int cpuid)
{
	if (cpuid < ARRAY_SIZE(pm_procs_all))
		return &pm_procs_all[cpuid];

	return NULL;
}
