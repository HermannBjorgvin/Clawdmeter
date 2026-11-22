# Waveshare ESP32-C6-LCD-1.47 (non-touch)

Experimental port for the non-touch board (SKU 28563 / 30381),
not the similarly named C6-Touch model. The ST7789 is 172×320 with a
34-column offset; it is used in landscape (320×172). There is no touch,
battery gauge, IMU or speaker. BOOT on GPIO9 controls brightness and opens
the pairing window when held for about three seconds and released.

The Waveshare documentation recommends no more than 50% backlight; the HAL
scales brightness accordingly. `huge_app.csv` fits the 4 MB flash without
OTA slots. The RGB LED and SD card are not used; SD CS is held high because the card
shares the display SPI bus. On a contributor-reported non-touch unit with ESP32-C6FH8 and 8 MB flash,
startup, BOOT pairing gesture and color appearance were checked. BLE connection
and full UI validation are still needed; the official documentation lists 4 MB.

Sources: https://docs.waveshare.com/ESP32-C6-LCD-1.47/ and the linked
Waveshare Arduino example package.
