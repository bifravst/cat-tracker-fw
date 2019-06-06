/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <dk_buttons_and_leds.h>
#include <zephyr.h>
#include <stdio.h>
#include <uart.h>
#include <string.h>

static void buttons_leds_init(void)
{
	#if defined(CONFIG_DK_LIBRARY)
		int err;

		err = dk_leds_init();
		if (err) {
			printk("Could not initialize leds, err code: %d\n", err);
		}

		err = dk_set_leds_state(0x00, DK_ALL_LEDS_MSK);
		if (err) {
			printk("Could not set leds state, err code: %d\n", err);
		}
	#endif
}

void main(void)
{
	printk("The application has started\n");
	buttons_leds_init();
	//gps_init();
}
