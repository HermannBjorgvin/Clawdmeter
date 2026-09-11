#pragma once
#include <stdint.h>

// 24 hours of environmental-sensor history in an NVS-backed ring buffer.
//
// 96 slots x 15 minutes = 24 h. Each slot holds the mean of every reading
// taken during that quarter-hour. The ring is flushed to NVS whenever it
// rolls to a new slot (~96 writes/day of a 588-byte blob — well inside NVS
// wear budget), so the trend survives a reboot or an unplug.
//
// This board has no RTC: wall-clock time only arrives with the daemon's
// {"src":"env"} payload over BLE. The ring is therefore indexed by an
// absolute 15-minute bucket number derived from that clock when we have it,
// and by a free-running millis() bucket when we don't — see
// sensor_hist_set_time(), which re-anchors the ring the moment real time
// shows up and rolls it forward over whatever we slept through.

#define SH_SLOTS      96            // 96 x 15 min = 24 h
#define SH_SLOT_SECS  (15 * 60)

enum sh_metric_t {
    SH_TEMP,     // deg C
    SH_HUM,      // % RH   (BME280 only)
    SH_PRESS,    // hPa
    SH_METRIC_COUNT,
};

void sensor_hist_init(void);              // restore the ring from NVS
void sensor_hist_tick(void);              // call from loop(); rolls + persists
void sensor_hist_set_time(long epoch_utc);// daemon clock; re-anchors the ring

// Feed a reading. Averaged into the slot currently being filled, which is
// also the newest point of the trace — so the right-hand edge of the graph
// tracks the live value.
void sensor_hist_sample(float temp_c, bool has_hum, float hum_pct, float press_hpa);

// Copy one metric oldest-first into out[0..n-1] (n should be SH_SLOTS).
// Slots with no reading come back as NAN. Returns the number of real samples.
int  sensor_hist_series(sh_metric_t m, float* out, int n);

// Minutes of wall time spanned by the samples we actually hold, 0 if none.
int  sensor_hist_span_mins(void);

// True once the ring is anchored to the daemon's wall clock rather than to
// a free-running millis() counter.
bool sensor_hist_time_is_real(void);

void sensor_hist_clear(void);             // wipe RAM + NVS copy

// QA only (serial "histfill"): synthesize a plausible 24 h trace so the trend
// graphs can be laid out and screenshotted without waiting a day for real
// data. Overwrites whatever is in the ring, NVS copy included.
void sensor_hist_debug_fill(void);
