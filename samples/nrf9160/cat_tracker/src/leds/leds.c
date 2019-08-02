#include <zephyr.h>
#include <leds.h>
#include <dk_buttons_and_leds.h>

void leds_init(void)
{
#if defined(CONFIG_DK_LIBRARY)
	int err = 0;

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

void led_notif_lte(bool connected)
{
#if defined(CONFIG_DK_LIBRARY)
	if (connected) {
		dk_set_led_on(DK_LED1);
	} else {
		dk_set_led_off(DK_LED1);
	}
#endif
}

void led_notif_gps_search(bool searching)
{
#if defined(CONFIG_DK_LIBRARY)
	if (searching) {
		dk_set_led_on(DK_LED2);
	} else {
		dk_set_led_off(DK_LED2);
	}
#endif
}

void led_notif_publish()
{
#if defined(CONFIG_DK_LIBRARY)
	dk_set_led_on(DK_LED3);
	k_sleep(500);
	dk_set_led_off(DK_LED3);
#endif
}