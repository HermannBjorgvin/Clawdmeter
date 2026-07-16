#pragma once

#include <string.h>

enum SerialLineType {
    SERIAL_LINE_UNKNOWN,
    SERIAL_LINE_SCREENSHOT,
    SERIAL_LINE_BUZZ,
    SERIAL_LINE_IDENTIFY,
    SERIAL_LINE_USAGE_JSON,
};

inline SerialLineType classify_serial_line(const char* line) {
    if (strcmp(line, "screenshot") == 0) return SERIAL_LINE_SCREENSHOT;
    if (strcmp(line, "buzz") == 0) return SERIAL_LINE_BUZZ;
    if (strcmp(line, "identify") == 0) return SERIAL_LINE_IDENTIFY;
    if (line[0] == '{') return SERIAL_LINE_USAGE_JSON;
    return SERIAL_LINE_UNKNOWN;
}
