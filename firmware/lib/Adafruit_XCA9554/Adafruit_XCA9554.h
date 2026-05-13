#ifndef ADAFRUIT_XCA9554_H
#define ADAFRUIT_XCA9554_H

#include "Adafruit_BusIO_Register.h"
#include "Adafruit_I2CDevice.h"
#include <Arduino.h>
#include <Wire.h>

#define XCA9554_REG_INPUT_PORT 0x00
#define XCA9554_REG_OUTPUT_PORT 0x01
#define XCA9554_REG_POLARITY_INVERT 0x02
#define XCA9554_REG_CONFIG 0x03

/**! Class for adding generic pin control to xCA9554 GPIO expander */
class Adafruit_XCA9554 {
public:
  Adafruit_XCA9554();

  bool begin(uint8_t i2c_addr, TwoWire *wire = &Wire);
  bool pinMode(uint8_t pin, uint8_t mode);
  bool digitalWrite(uint8_t pin, bool val);
  bool digitalRead(uint8_t pin);

private:
  Adafruit_I2CDevice *i2c_dev;
  Adafruit_BusIO_Register *input_port_reg;
  Adafruit_BusIO_Register *output_port_reg;
  Adafruit_BusIO_Register *polarity_inversion_reg;
  Adafruit_BusIO_Register *config_reg;
};

#endif // ADAFRUIT_XCA9554_H
