// millis()-based wrappers over the pure usage_rate / signals cores. Kept in
// their own file so the native tests can build the cores without Arduino.
#include <Arduino.h>
#include "usage_rate.h"
#include "signals.h"

int usage_rate_group(void) { return usage_rate_group_at(millis()); }
int usage_rate_base_group(void) { return usage_rate_base_group_at(millis()); }

SignalsUpdate signals_ingest(const UsageData* d) { return signals_ingest_at(d, millis()); }
int      signals_activity(void)        { return signals_activity_at(millis()); }
