// Native entry point — stands in for the Arduino runtime. loop()'s own
// delay(5) paces the loop the same way it does on hardware.
#include <Arduino.h>
#include "sim_platform.h"

int main(void) {
    printf(
        "Clawdmeter simulator\n"
        "  mouse          touch (tap toggles splash/usage)\n"
        "  space          play/pause scenario    left/right step    1-9 jump\n"
        "  d              toggle BLE link\n"
        "  b / n (hold)   BOOT / secondary button    p  PWR button\n"
        "  c              toggle charging            - / =  battery down/up\n"
        "  s              screenshot                 esc  quit\n\n");
    setup();
    while (!sim_should_quit()) {
        sim_pump();
        loop();
    }
    return 0;
}
