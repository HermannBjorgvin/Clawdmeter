#pragma once
// Minimal Arduino compatibility layer for the native simulator build.
// Covers only what the shared sources actually use: millis/micros/delay, Serial,
// and the setup()/loop() entry points (driven by sim_main.cpp).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);

class SimSerial {
public:
    void begin(unsigned long baud) { (void)baud; }
    // Fed by SIM_CMDS (sim_platform.cpp) so headless runs can type serial
    // commands (`clawd lurk`, `screenshot`, ...) at scripted times.
    int  available(void);
    int  read(void);
    void feed(const char* line);   // queue a line (newline appended)
    size_t write(const uint8_t* buf, size_t len);
    void flush(void) { fflush(stdout); }
    void print(const char* s) { fputs(s, stdout); }
    void println(const char* s) { puts(s); }
    void println(void) { putchar('\n'); }
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};
extern SimSerial Serial;

void setup(void);
void loop(void);
