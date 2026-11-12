#pragma once

// Copy on the meters whose width decides the layout: ui.cpp draws these
// strings and layout.cpp sizes the screen for the widest of them, so a
// rewording here is checked by test_layout on every panel size.

// Pairing hint: heading, two steps, and a note line, per board kind / state
// (ui.cpp apply_pair_text).
static const char* const PAIR_TEXT_PWR[3]     = { "To pair", "hold the power button", "for 3 seconds, then release" };
static const char* const PAIR_TEXT_TOUCH[3]   = { "To pair", "touch and hold here", "for 5 seconds" };
static const char* const PAIR_TEXT_CLEARED[3] = { "Ready to pair", "choose Clawdmeter", "on your computer" };
static const char* const PAIR_TEXT_HOLDING    = "keep holding...";

// Card pills: session, weekly, then the spend-capped account's spending and
// period. The short set is for panels too narrow for the long one.
static const char* const PILL_TEXT[4]       = { "Session", "Weekly", "Spending", "Period" };
static const char* const PILL_TEXT_SHORT[4] = { "5h", "7d", "Spend", "Period" };

// Widest text a card's reset line shows: the weekly window's last day, the
// spend-capped account's caption, and its period line (its widest pace word and date).
static const char* const RESET_TEXT_WIDEST[] = { "Resets in 23h 59m", "Resets in 6d 23h", "of spending limit" };
#define PERIOD_TEXT_WIDEST "Under pace - Resets May 30"
#define PERIOD_TEXT_SHORT_WIDEST "Resets May 30"   // where no size fits the pace word too

// Status-line words every panel must fit (the whimsical verbs are skipped
// when they don't): "<spinner> <word>…".
static const char* const STATUS_TEXT_REQUIRED[] = {
    "\xE2\x9C\xBB Connected\xE2\x80\xA6", "\xE2\x9C\xBB Listening\xE2\x80\xA6",
    "\xE2\x9C\xBB Waiting\xE2\x80\xA6",   "\xE2\x9C\xBB No data\xE2\x80\xA6",
};
