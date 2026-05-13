#include <Adafruit_XCA9554.h>

// Create an instance of the Adafruit_XCA9554 class
Adafruit_XCA9554 expander;

void setup() {
  Serial.begin(115200);

  while (!Serial) delay(10); // Wait for port to open if native USB

  Serial.println("PCA/TCA9554 Simple Test Sketch");

  // Begin communication with the expander
  if (!expander.begin(0x20)) { // Replace with actual I2C address if different
    Serial.println("Failed to find XCA9554 chip");
    while (1);
  }

  // Configure pin 0 as an output
  expander.pinMode(0, OUTPUT);

  // Configure pin 1 as an input
  expander.pinMode(1, INPUT);
}

uint32_t previousMillis = 0; // stores last time LED was updated
uint32_t interval = 500; // interval at which to blink (milliseconds)
bool pinState = false;

void loop() {
  uint32_t currentMillis = millis();

  // Blink LED on pin 0
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    expander.digitalWrite(0, pinState);
    if (pinState) 
      Serial.println("Blink!");
    pinState = !pinState;    // Toggle LED output
  }

  // Check if pin 1 is pulled low (button pressed)
  if (expander.digitalRead(1) == LOW) {
    Serial.println("Button pressed");
  }

  delay(10); // debounce delay
}
