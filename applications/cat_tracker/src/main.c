#include <zephyr.h>
#include <stdio.h>
#include <uart.h>
#include <string.h>
#include <logging/log.h>
#include <logging/log_ctrl.h>
#include <misc/reboot.h>
#include <device.h>
#include <sensor.h>
#include <gps.h>
#include <gps_controller.h>
#include <ui.h>
#include <net/cloud.h>
#include <cloud_codec.h>
#include <lte_lc.h>
#include <stdlib.h>
#include <modem_info.h>
#include <time.h>
#include <nrf_socket.h>
#include <net/socket.h>
#include "version.h"

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <dfu/mcuboot.h>
#endif

enum lte_conn_actions {
	LTE_INIT,
	LTE_UPDATE,
};

enum error_type {
	ERROR_BSD_RECOVERABLE,
	ERROR_BSD_IRRECOVERABLE,
	ERROR_SYSTEM_FAULT,
	ERROR_CLOUD
};

struct cloud_data_gps cir_buf_gps[CONFIG_CIRCULAR_SENSOR_BUFFER_MAX];

struct cloud_data cloud_data = { .gps_timeout = 60,
				 .active = true,
				 .active_wait = 60,
				 .passive_wait = 60,
				 .movement_timeout = 3600,
				 .accel_threshold = 100,
				 .gps_found = false };

struct k_timer governing_timer;

struct cloud_data_time cloud_data_time;

struct modem_param_info modem_param;

static struct cloud_backend *cloud_backend;
static bool queued_entries;

static int rsrp;
static int head_cir_buf;
static int num_queued_entries;

static struct k_work cloud_send_cfg_work;
static struct k_work cloud_pairing_routine_work;
static struct k_work cloud_update_routine_work;

K_SEM_DEFINE(accel_trig_sem, 0, 1);
K_SEM_DEFINE(gps_timeout_sem, 0, 1);

#define CLOUD_POLL_STACKSIZE 4096
#define CLOUD_POLL_PRIORITY 7

void error_handler(enum error_type err_type, int err_code)
{
#if !defined(CONFIG_DEBUG) && defined(CONFIG_REBOOT)
	LOG_PANIC();
	sys_reboot(0);
#else

	switch (err_type) {
	case ERROR_BSD_RECOVERABLE:
		printk("Error of type ERROR_BSD_RECOVERABLE: %d\n", err_code);
		ui_led_set_pattern(UI_LED_ERROR_BSD_REC);
		break;
	case ERROR_BSD_IRRECOVERABLE:
		printk("Error of type ERROR_BSD_IRRECOVERABLE: %d\n", err_code);
		ui_led_set_pattern(UI_LED_ERROR_BSD_IRREC);
		break;

	case ERROR_SYSTEM_FAULT:
		printk("Error of type ERROR_SYSTEM_FAULT: %d\n", err_code);
		ui_led_set_pattern(UI_LED_ERROR_SYSTEM_FAULT);
		break;

	case ERROR_CLOUD:
		printk("Error of type ERROR_CLOUD: %d\n", err_code);
		ui_led_set_pattern(UI_LED_ERROR_CLOUD);
		break;

	default:
		printk("Unknown error type: %d, code: %d\n", err_type,
		       err_code);
		ui_led_set_pattern(UI_LED_ERROR_UNKNOWN);
		break;
	}

	while (true) {
		k_cpu_idle();
	}
#endif
}

void k_sys_fatal_error_handler(unsigned int reason, const z_arch_esf_t *esf)
{
	ARG_UNUSED(esf);

	LOG_PANIC();
	printk("Running main.c error handler");
	error_handler(ERROR_SYSTEM_FAULT, reason);
	CODE_UNREACHABLE;
}

void cloud_error_handler(int err)
{
	error_handler(ERROR_CLOUD, err);
}

static int check_active_wait(void)
{
	if (!cloud_data.active) {
		return cloud_data.passive_wait;
	}

	return cloud_data.active_wait;
}

static int parse_time_entries(char *datetime_string, int min, int max)
{
	char buf[50];

	for (int i = min; i < max + 1; i++) {
		buf[i - min] = datetime_string[i];
	}

	return atoi(buf);
}

#if defined(CONFIG_MODEM_INFO)
static void parse_modem_time_data(void)
{
	struct tm info;

	info.tm_year = parse_time_entries(
		modem_param.network.date_time.value_string, 0, 1) + 2000 - 1900;
	info.tm_mon  = parse_time_entries(
		modem_param.network.date_time.value_string, 3, 4) - 1;
	info.tm_mday = parse_time_entries(
		modem_param.network.date_time.value_string, 6, 7);
	info.tm_hour = parse_time_entries(
		modem_param.network.date_time.value_string, 9, 10);
	info.tm_min  = parse_time_entries(
		modem_param.network.date_time.value_string, 12, 13);
	info.tm_sec  = parse_time_entries(
		modem_param.network.date_time.value_string, 15, 16);

	printk("%d/%d/%d,%d:%d:%d\n", info.tm_mday, info.tm_mon,
	       (info.tm_year - 100), info.tm_hour, info.tm_min, info.tm_sec);

	cloud_data_time.epoch = mktime(&info);
	cloud_data_time.update_time = k_uptime_get();

}
#endif

static void set_current_time(struct gps_data gps_data)
{
	struct tm info;

	info.tm_year = gps_data.pvt.datetime.year - 1900;
	info.tm_mon = gps_data.pvt.datetime.month - 1;
	info.tm_mday = gps_data.pvt.datetime.day;
	info.tm_hour = gps_data.pvt.datetime.hour;
	info.tm_min = gps_data.pvt.datetime.minute;
	info.tm_sec = gps_data.pvt.datetime.seconds;

	cloud_data_time.epoch = mktime(&info);
	cloud_data_time.update_time = k_uptime_get();
}

static double get_accel_thres(void)
{
	double accel_threshold_double;

	if (cloud_data.accel_threshold == 0) {
		accel_threshold_double = 0;
	} else {
		accel_threshold_double = cloud_data.accel_threshold / 10;
	}

	return accel_threshold_double;
}

static void populate_gps_buffer(struct gps_data gps_data)
{
	cloud_data.gps_found = true;

	head_cir_buf += 1;
	if (head_cir_buf == CONFIG_CIRCULAR_SENSOR_BUFFER_MAX - 1) {
		head_cir_buf = 0;
	}

	cir_buf_gps[head_cir_buf].longitude = gps_data.pvt.longitude;
	cir_buf_gps[head_cir_buf].latitude = gps_data.pvt.latitude;
	cir_buf_gps[head_cir_buf].altitude = gps_data.pvt.altitude;
	cir_buf_gps[head_cir_buf].accuracy = gps_data.pvt.accuracy;
	cir_buf_gps[head_cir_buf].speed = gps_data.pvt.speed;
	cir_buf_gps[head_cir_buf].heading = gps_data.pvt.heading;
	cir_buf_gps[head_cir_buf].gps_timestamp = k_uptime_get();
	cir_buf_gps[head_cir_buf].queued = true;

	printk("Entry: %d in gps_buffer filled", head_cir_buf);
}

#if defined(CONFIG_MODEM_INFO)
static int get_voltage_level(void)
{

	cloud_data.bat_voltage = modem_param.device.battery.value;
	cloud_data.bat_timestamp = k_uptime_get();

	return 0;
}
#endif

static void cloud_pair(void)
{
	int err;

	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_TOPIC_PAIR,
		.buf = "",
		.len = 0 };

	err = cloud_send(cloud_backend, &msg);
	if (err != 0) {
		printk("Cloud send failed, err: %d\n", err);
	}
}

static void cloud_send_cfg(void)
{
	int err;

	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_TOPIC_MSG,
	};

	err = cloud_encode_cfg_data(&msg, &cloud_data);
	if (err != 0) {
		printk("Error enconding configurations %d\n", err);
		return;
	}

	err = cloud_send(cloud_backend, &msg);
	if (err != 0) {
		printk("Cloud send failed, err: %d\n", err);
		return;
	}
}

static void cloud_send_sensor_data(void)
{
	int err;

	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_TOPIC_MSG,
	};

	err = get_voltage_level();
	if (err != 0) {
		printk("Error requesting voltage level %d\n", err);
		return;
	}

	err = cloud_encode_sensor_data(&msg, &cloud_data,
				       &cir_buf_gps[head_cir_buf],
				       &cloud_data_time);
	if (err != 0) {
		printk("Error enconding message %d\n", err);
		return;
	}

	err = cloud_send(cloud_backend, &msg);
	if (err != 0) {
		printk("Cloud send failed, err: %d\n", err);
		return;
	}

	cloud_data.gps_found = false;
}

#if defined(CONFIG_MODEM_INFO)
static void cloud_send_modem_data(int inc_dyn_data)
{
	int err;

	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_TOPIC_MSG,
	};

	err = modem_info_params_get(&modem_param);
	if (err != 0) {
		printk("Error getting modem_info: %d\n", err);
		return;
	}

	err = cloud_encode_modem_data(&msg, &modem_param, inc_dyn_data, rsrp,
				      &cloud_data_time);
	if (err != 0) {
		printk("Error encoding modem data");
		return;
	}

	err = cloud_send(cloud_backend, &msg);
	if (err != 0) {
		printk("Cloud send failed, err: %d\n", err);
		return;
	}
}
#endif

static void cloud_send_buffered_data(void)
{
	int err;

	struct cloud_msg msg = {
		.qos = CLOUD_QOS_AT_MOST_ONCE,
		.endpoint.type = CLOUD_EP_TOPIC_BATCH,
	};

	for (int i = 0; i < CONFIG_CIRCULAR_SENSOR_BUFFER_MAX; i++) {
		if (cir_buf_gps[i].queued) {
			queued_entries = true;
			num_queued_entries++;
		}
	}

	while (num_queued_entries > 0 && queued_entries) {
		err = cloud_encode_gps_buffer(&msg, cir_buf_gps,
						&cloud_data_time);
		if (err != 0) {
			printk("Error encoding circular buffer: %d\n", err);
			goto end;
		}

		err = cloud_send(cloud_backend, &msg);
		if (err != 0) {
			printk("Cloud send failed, err: %d\n", err);
			goto end;
		}

		num_queued_entries -= CONFIG_CIRCULAR_SENSOR_BUFFER_MAX;
	}

end:
	num_queued_entries = 0;
	queued_entries = false;
}

static void cloud_pairing_routine(void)
{
	ui_led_set_pattern(UI_CLOUD_CONNECTED);

#if defined(CONFIG_CLOUD_PAIR)
	cloud_pair();
#endif

#if defined(CONFIG_MODEM_INFO)
	cloud_send_modem_data(true);
#endif
}

static void cloud_update_routine(void)
{
	ui_led_set_pattern(UI_CLOUD_CONNECTED);

#if defined(CONFIG_SENSOR_DATA_SEND)
	cloud_send_sensor_data();
#endif

#if defined(CONFIG_MODEM_INFO)
	cloud_send_modem_data(false);
#endif

#if defined(CONFIG_BUFFERED_DATA_SEND)
	cloud_send_buffered_data();
#endif
}

static void cloud_send_cfg_work_fn(struct k_work *work)
{
	cloud_send_cfg();
}

static void cloud_pairing_routine_work_fn(struct k_work *work)
{
	cloud_pairing_routine();
}

static void cloud_update_routine_work_fn(struct k_work *work)
{
	cloud_update_routine();
}

static void work_init(void)
{
	k_work_init(&cloud_send_cfg_work, cloud_send_cfg_work_fn);
	k_work_init(&cloud_pairing_routine_work, cloud_pairing_routine_work_fn);
	k_work_init(&cloud_update_routine_work, cloud_update_routine_work_fn);
}

static void adxl362_trigger_handler(struct device *dev,
				    struct sensor_trigger *trig)
{
	static struct sensor_value accel[3];

	switch (trig->type) {
	case SENSOR_TRIG_THRESHOLD:

		if (sensor_sample_fetch(dev) < 0) {
			printk("Sample fetch error\n");
			return;
		}

		sensor_channel_get(dev, SENSOR_CHAN_ACCEL_X, &accel[0]);
		sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Y, &accel[1]);
		sensor_channel_get(dev, SENSOR_CHAN_ACCEL_Z, &accel[2]);

		double x = sensor_value_to_double(&accel[0]);
		double y = sensor_value_to_double(&accel[1]);
		double z = sensor_value_to_double(&accel[2]);

		if ((abs(x) > get_accel_thres()) ||
		    (abs(y) > get_accel_thres()) ||
		    (abs(z) > get_accel_thres())) {
			cloud_data.acc[0] = x;
			cloud_data.acc[1] = y;
			cloud_data.acc[2] = z;
			cloud_data.acc_timestamp = k_uptime_get();
			k_sem_give(&accel_trig_sem);
		}

		break;
	default:
		printk("Unknown trigger\n");
	}
}

static void gps_trigger_handler(struct device *dev, struct gps_trigger *trigger)
{
	static u32_t fix_count;
	struct gps_data gps_data;

	ARG_UNUSED(trigger);

	if (++fix_count < CONFIG_GPS_CONTROL_FIX_COUNT) {
		return;
	}

	fix_count = 0;

	printk("gps control handler triggered!\n");

	gps_channel_get(dev, GPS_CHAN_PVT, &gps_data);
	set_current_time(gps_data);
	populate_gps_buffer(gps_data);
	gps_control_stop(1);
}

static void adxl362_init(void)
{
	struct device *dev = device_get_binding(DT_INST_0_ADI_ADXL362_LABEL);

	if (dev == NULL) {
		printk("Device get binding device\n");
		return;
	}

	if (IS_ENABLED(CONFIG_ADXL362_TRIGGER)) {
		struct sensor_trigger trig = { .chan = SENSOR_CHAN_ACCEL_XYZ };

		trig.type = SENSOR_TRIG_THRESHOLD;
		if (sensor_trigger_set(dev, &trig, adxl362_trigger_handler)) {
			printk("Trigger set error\n");
			return;
		}
	}
}

void cloud_event_handler(const struct cloud_backend *const backend,
			 const struct cloud_event *const evt, void *user_data)
{
	ARG_UNUSED(user_data);

	int err;

	switch (evt->type) {
	case CLOUD_EVT_CONNECTED:
		printk("CLOUD_EVT_CONNECTED\n");

		k_work_submit(&cloud_pairing_routine_work);

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
		boot_write_img_confirmed();
#endif
		break;
	case CLOUD_EVT_READY:
		printk("CLOUD_EVT_READY\n");
		break;
	case CLOUD_EVT_DISCONNECTED:
		printk("CLOUD_EVT_DISCONNECTED\n");
		break;
	case CLOUD_EVT_ERROR:
		printk("CLOUD_EVT_ERROR\n");
		break;
	case CLOUD_EVT_FOTA_DONE:
		printk("CLOUD_EVT_FOTA_DONE");
		cloud_disconnect(cloud_backend);
		sys_reboot(0);
		break;
	case CLOUD_EVT_DATA_SENT:
		printk("CLOUD_EVT_DATA_SENT\n");
		break;
	case CLOUD_EVT_DATA_RECEIVED:
		printk("CLOUD_EVT_DATA_RECEIVED\n");

		err = cloud_decode_response(evt->data.msg.buf, &cloud_data);
		if (err != 0) {
			printk("Could not decode response %d\n", err);
		}

		k_work_submit(&cloud_send_cfg_work);

		break;
	case CLOUD_EVT_PAIR_REQUEST:
		printk("CLOUD_EVT_PAIR_REQUEST\n");
		break;
	case CLOUD_EVT_PAIR_DONE:
		printk("CLOUD_EVT_PAIR_DONE\n");
		break;
	default:
		printk("Unknown cloud event type: %d\n", evt->type);
		break;
	}
}

void cloud_poll(void)
{
	int err;

connect:
	err = cloud_connect(cloud_backend);
	if (err) {
		printk("cloud_connect failed: %d\n", err);
	}

	struct pollfd fds[] = {
		{
			.fd = cloud_backend->config->socket,
			.events = POLLIN
		}
	};

	while (true) {
		err = poll(fds, ARRAY_SIZE(fds),
			K_SECONDS(CONFIG_MQTT_KEEPALIVE));

		if (err < 0) {
			printk("poll() returned an error: %d\n", err);
			error_handler(ERROR_CLOUD, err);
			continue;
		}

		if (err == 0) {
			cloud_ping(cloud_backend);
			continue;
		}

		if ((fds[0].revents & POLLIN) == POLLIN) {
			cloud_input(cloud_backend);
		}

		if ((fds[0].revents & POLLNVAL) == POLLNVAL) {
			printk("Socket error: POLLNVAL\n");
			error_handler(ERROR_CLOUD, -EIO);
			return;
		}

		if ((fds[0].revents & POLLHUP) == POLLHUP) {
			printk("Socket error: POLLHUP\n");
			error_handler(ERROR_CLOUD, -EIO);
			return;
		}

		if ((fds[0].revents & POLLERR) == POLLERR) {
			printk("Socket error: POLLERR\n");
			error_handler(ERROR_CLOUD, -EIO);
			return;
		}
	}

	cloud_disconnect(cloud_backend);
	goto connect;
}

K_THREAD_DEFINE(cloud_poll_thread, CLOUD_POLL_STACKSIZE,
		cloud_poll, NULL, NULL, NULL,
		CLOUD_POLL_PRIORITY, 0, K_FOREVER);

static void lte_connect(enum lte_conn_actions action)
{
	int err;

	enum lte_lc_nw_reg_status nw_reg_status;

	ui_led_set_pattern(UI_LTE_CONNECTING);

	if (action == LTE_INIT) {
		if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
			/* Do nothing, modem is already turned on
			 * and connected.
			 */
		} else {
			printk("Connecting to LTE network. ");
			printk("This may take several minutes.\n");
			err = lte_lc_init_and_connect();
			if (err == -ETIMEDOUT) {
				printk("LTE link could not be established.\n");
				goto gps_mode;
			}
		}
	} else if (action == LTE_UPDATE) {
		err = lte_lc_nw_reg_status_get(&nw_reg_status);
		if (err != 0) {
			printk("lte_lc_nw_reg_status error: %d\n", err);
			goto gps_mode;
		}

		switch (nw_reg_status) {
		case LTE_LC_NW_REG_REGISTERED_HOME:
			printk("REGISTERED TO HOME NETWORK\n");
			return;
		case LTE_LC_NW_REG_REGISTERED_ROAMING:
			printk("REGISTERED TO ROAMING NETWORK\n");
			return;
		default:
			printk("LTE not connected.\n");
			printk("Connecting to LTE network. ");
			printk("This may take several minutes.\n");
			err = lte_lc_init_and_connect();
			if (err != 0) {
				printk("LTE link could not be established.\n");
				goto gps_mode;
			}
			break;
		}
	}

	printk("LTE connected!\nFetching modem time...\n");

#if defined(CONFIG_MODEM_INFO)
	k_sleep(1000);

	err = modem_info_params_get(&modem_param);
	if (err != 0) {
		printk("Error getting modem_info: %d", err);
	}

	parse_modem_time_data();
#endif
	ui_led_set_pattern(UI_LTE_CONNECTED);

	k_thread_start(cloud_poll_thread);

	return;

gps_mode:

	k_thread_abort(cloud_poll_thread);

	lte_lc_gps_nw_mode();

}

#if defined(CONFIG_MODEM_INFO)
static int modem_data_init(void)
{
	int err;

	err = modem_info_init();
	if (err != 0) {
		return err;
	}

	err = modem_info_params_init(&modem_param);
	if (err != 0) {
		return err;
	}

	return 0;
}
#endif

void main(void)
{
	int err;

	printk("The cat tracker has started\n");
	printk("Version: %s\n", DEVICE_APP_VERSION);

	cloud_backend = cloud_get_binding("BIFRAVST_CLOUD");
	__ASSERT(cloud_backend != NULL, "Bifravst Cloud backend not found");

	err = cloud_init(cloud_backend, cloud_event_handler);
	if (err) {
		printk("Cloud backend could not be initialized, error: %d\n ",
		       err);
		cloud_error_handler(err);
	}

#if defined(CONFIG_USE_UI_MODULE)
	ui_init();
#endif

#if defined(CONFIG_MODEM_INFO)
	modem_data_init();
#endif
	work_init();
	lte_connect(LTE_INIT);
	adxl362_init();
	gps_control_init(gps_trigger_handler);

check_mode:

	if (!cloud_data.active) {
		if (!k_sem_take(&accel_trig_sem, K_FOREVER)) {
			printk("Wooow, the cat is moving!\n");
		}
	}

	gps_control_start(1);
	if (k_sem_take(&gps_timeout_sem, K_SECONDS(cloud_data.gps_timeout))) {
		gps_control_stop(1);
	}

	k_sleep(K_SECONDS(check_active_wait()));

	lte_connect(LTE_UPDATE);
	cloud_update_routine();

goto check_mode;

}
