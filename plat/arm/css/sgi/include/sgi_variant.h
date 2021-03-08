/*
 * Copyright (c) 2018, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SGI_VARIANT_H
#define SGI_VARIANT_H

/* SSC_VERSION values for SGI575 */
#define SGI575_SSC_VER_PART_NUM		0x0783

/* SID Version values for SGI-Clark */
#define SGI_CLARK_SID_VER_PART_NUM		0x0786

/* Structure containing SGI platform variant information */
typedef struct sgi_platform_info {
	unsigned int platform_id;	/* Part Number of the platform */
	unsigned int config_id;		/* Config Id of the platform */
} sgi_platform_info_t;

extern sgi_platform_info_t sgi_plat_info;

#endif /* SGI_VARIANT_H */
