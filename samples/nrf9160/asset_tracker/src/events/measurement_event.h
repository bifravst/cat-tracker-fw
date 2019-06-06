#include "event_manager.h"

struct measurement_event {
    struct event_header header;

    s8_t value1;
    s16_t value2;
    s32_t value3;

};

EVENT_TYPE_DECLARE(measurement_event);