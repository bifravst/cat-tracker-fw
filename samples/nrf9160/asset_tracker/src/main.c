/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <dk_buttons_and_leds.h>
#include <zephyr.h>
//#include <gps.h>
#include <stdio.h>
#include <uart.h>
#include <string.h>

// /**@brief Recoverable BSD library error. */
// void bsd_recoverable_error_handler(uint32_t err)
// {
// 	printk("bsdlib recoverable error: %u\n", err);
// }

// /**@brief Irrecoverable BSD library error. */
// void bsd_irrecoverable_error_handler(uint32_t err)
// {
// 	printk("bsdlib irrecoverable error: %u\n", err);

// 	__ASSERT_NO_MSG(false);
// }

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

// static void gps_init(void)
// {
// 	int err;
// 	struct device *gps_dev = device_get_binding(CONFIG_GPS_DEV_NAME);
// 	struct gps_trigger gps_trig = {
// 		.type = GPS_TRIG_DATA_READY,
// 	};

// 	if (gps_dev == NULL) {
// 		printk("Could not get %s device\n", CONFIG_GPS_DEV_NAME);
// 		return;
// 	}
// 	printk("GPS device found\n");

// 	if (IS_ENABLED(CONFIG_GPS_TRIGGER)) {
// 		err = gps_trigger_set(gps_dev, &gps_trig,
// 				gps_trigger_handler);

// 		if (err) {
// 			printk("Could not set trigger, error code: %d\n", err);
// 			return;
// 		}
// 	}

// 	err = gps_sample_fetch(gps_dev);
// 	__ASSERT(err == 0, "GPS sample could not be fetched.");

// 	err = gps_channel_get(gps_dev, GPS_CHAN_NMEA, &nmea_data);
// 	__ASSERT(err == 0, "GPS sample could not be retrieved.");
// }

// static void gps_data_pull(void)
// {

// }

void main(void)
{
	printk("The application has started\n");
	buttons_leds_init();
	//gps_init();
}
