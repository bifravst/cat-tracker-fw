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
#include <event_manager.h>
#include <logging/log.h>
#include <measurement_event.h> //configuration file for event manager
#include <misc/reboot.h>
#include <net/mqtt.h>
#include <net/socket.h>
#include <lte_lc.h>

void bsd_recoverable_error_handler(uint32_t err)
{
	printk("bsdlib recoverable error: %u\n", err);
}

/**@brief Irrecoverable BSD library error. */
void bsd_irrecoverable_error_handler(uint32_t err)
{
	printk("bsdlib irrecoverable error: %u\n", err);

	__ASSERT_NO_MSG(false);
}

LOG_MODULE_REGISTER(MODULE);

static void modem_configure(void)
{
#if defined(CONFIG_LTE_LINK_CONTROL)
	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already turned on
		 * and connected.
		 */
	} else {
		int err;

		printk("LTE Link Connecting ...\n");
		err = lte_lc_init_and_connect();
		__ASSERT(err == 0, "LTE link could not be established.");
		printk("LTE Link Connected!\n");
	}
#endif
}

static void generate_event(void) {
	struct measurement_event *event = new_measurement_event();

	event->type = GPS_REQ_DATA; //this value should be replace by actual GPS data

	EVENT_SUBMIT(event);
}

#if defined(CONFIG_DK_LIBRARY)
static void button_handler(u32_t button_state, u32_t has_changed)
{
	uint32_t button = button_state & has_changed;

	if (button == DK_BTN1) {
		printk("BUTTON 1 on the dk was pressed, GPS data requested\n");
		generate_event();
	}

}
#endif

static void buttons_leds_init(void)
{
	#if defined(CONFIG_DK_LIBRARY)
		int err;

		err = dk_buttons_init(button_handler);
		if (err) {
			printk("Could not initialize buttons, err code: %d\n", err);
		}

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
	event_manager_init();
	modem_configure();

	// while(1) {
	// 	//this section needs to include some sort of power saving, we will get to that
	// }	
	// return 0;
}
