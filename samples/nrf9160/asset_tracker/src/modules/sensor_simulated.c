#include <measurement_event.h>


#define MODULE sensor_sim

static bool event_handler(const struct event_header *eh)    //function is called whenever the subscribed events is being processed
{
	if (is_measurement_event(eh)) {
		struct measurement_event *event = cast_sample_event(eh);

		s8_t v1 = event->value1;
		s16_t v2 = event->value2;
		s32_t v3 = event->value3;

		foo(v1, v2, v3);

        //Execute some action here

		return false;
	}

	return false;
}

EVENT_LISTENER(MODULE, event_handler);  //defining module as listener
EVENT_SUBSCRIBE(MODULE, measurement_event); //defining module as subscriber

/***
 * 
 * EVENT_SUBSCRIBE_EARLY - notification before other listeners
 * EVENT_SUBSCRIBE - standard notification
 * EVENT_SUBSCRIBE_FINAL - notification as last, final subscriber
 *