# Waveshare ESP32-C6-LCD-1.47 (non-touch)

Experimental port for the ESP32-C6FH4 4 MB board (SKU 28563 / 30381),
not the similarly named C6-Touch model. The ST7789 is 172×320 with a
34-column offset; it is used in landscape (320×172). There is no touch,
battery gauge, IMU or speaker. BOOT on GPIO9 controls brightness and opens
the pairing window when held for about three seconds and released.

The Waveshare documentation recommends no more than 50% backlight; the HAL
scales brightness accordingly. `huge_app.csv` fits the 4 MB flash without
OTA slots. The RGB LED and SD card are not used. Hardware validation is still
needed, including screen alignment, colors, BLE pairing and button behavior.

Sources: https://docs.waveshare.com/ESP32-C6-LCD-1.47/ and the linked
Waveshare Arduino example package.
