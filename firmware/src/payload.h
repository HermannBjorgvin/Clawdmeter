#pragma once
#include "data.h"

// Daemon → device JSON payload (one line written to the RX characteristic).
// Pure: no Arduino, no Serial, so it runs in the native unit tests.
//
// Every key is optional. Absent keys parse to the same defaults the firmware
// has always used, so an old daemon produces exactly the UsageData it always
// did, with the fast-clock fields reading "absent" (activity -1, event '\0',
// has_event_seq false, sessions -1, replay false).
//
//   s  sr  w  wr  st  c  b  m  x  acct  tp  pd  rd  t  tf  ok   (long-standing)
//   a   activity level 0-3 (clamped)          e   latest event, first char used
//   es  event sequence number (uint32)        n   active sessions (clamped 0..999)
//   rp  1 on a heartbeat replay               lv  liveliness 1-5 (clamped), -1 absent
//   aa  attention animation seconds 0-120 (clamped, 0 = off), -1 absent
//
// Returns false on malformed JSON; `*err` (if non-null) then points at a
// static description. `out` is fully overwritten on success, untouched on
// failure.
bool payload_parse(const char* json, UsageData* out, const char** err);
