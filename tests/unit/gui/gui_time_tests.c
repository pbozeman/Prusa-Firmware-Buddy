#include <stdint.h>

extern uint32_t tick;

uint32_t ticks_ms() { return tick; }

typedef void metric_t;
void metric_record_integer_at_time(metric_t *metric, uint32_t timestamp, int value) {}
