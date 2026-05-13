/*!
 * @file Adafruit_XCA9554.cpp
 *
 * @mainpage Adafruit TCA9554 or PCA9554 I/O Expander library
 *
 * @section intro_sec Introduction
 *
 * This is the documentation for Adafruit's xCA9554 driver for the
 * Arduino platform. It is designed specifically to work with
 * Adafruit dev and breakout boards that use TCA9554 or PCA9554
 *
 * This library allows you to communicate with the TCA/PCA9554 I/O expander chip
 * using I2C. The chip extends the number of I/O pins available for use in
 * your projects. It is capable of interfacing with both 3.3V and 5V logic
 * levels.
 *
 * @section dependencies Dependencies
 *
 * This library depends on the <a
 * href="https://github.com/adafruit/Adafruit_BusIO"> Adafruit BusIO library</a>
 * for low-level communication, and the standard Wire library for I2C
 * communication.
 *
 * @section author Author
 *
 * Written by Limor Fried/Ladyada for Adafruit Industries.
 *
 * @section license License
 *
 * BSD license, all text here must be included in any redistribution.
 */

#include "Adafruit_XCA9554.h"

/**
 * @brief Construct a new Adafruit_XCA9554::Adafruit_XCA9554 object.
 */
Adafruit_XCA9554::Adafruit_XCA9554()
    : i2c_dev(nullptr), input_port_reg(nullptr), output_port_reg(nullptr),
      polarity_inversion_reg(nullptr), config_reg(nullptr) {}

/**
 * @brief Initialize the XCA9554 chip.
 *
 * @param i2c_addr I2C address of the XCA9554 chip.
 * @param wire The I2C interface to use, defaults to Wire.
 * @return true if initialization is successful, false otherwise.
 */
bool Adafruit_XCA9554::begin(uint8_t i2c_addr, TwoWire *wire) {
  if (i2c_dev) {
    delete i2c_dev;
  }
  i2c_dev = new Adafruit_I2CDevice(i2c_addr, wire);
  if (!i2c_dev->begin()) {
    return false;
  }

  // Initialize register objects
  input_port_reg = new Adafruit_BusIO_Register(i2c_dev, XCA9554_REG_INPUT_PORT);
  output_port_reg =
      new Adafruit_BusIO_Register(i2c_dev, XCA9554_REG_OUTPUT_PORT);
  polarity_inversion_reg =
      new Adafruit_BusIO_Register(i2c_dev, XCA9554_REG_POLARITY_INVERT);
  config_reg = new Adafruit_BusIO_Register(i2c_dev, XCA9554_REG_CONFIG);

  // Configure all pins as inputs by default
  uint8_t configValue = 0xFF; // All pins as input
  if (!config_reg->write(configValue)) {
    return false;
  }

  return true;
}

/**
 * @brief Set the mode of a pin (INPUT/OUTPUT).
 *
 * @param pin The pin number (0-7) to set.
 * @param mode The mode to set the pin (INPUT/OUTPUT).
 * @returns Whether we successfully wrote the bit to the register
 */
bool Adafruit_XCA9554::pinMode(uint8_t pin, uint8_t mode) {
  // Validate parameters
  if (!i2c_dev || !config_reg || pin > 7 || (mode != INPUT && mode != OUTPUT)) {
    return false;
  }

  // Create a RegisterBits object for the specific pin
  Adafruit_BusIO_RegisterBits pinModeBit =
      Adafruit_BusIO_RegisterBits(config_reg, 1, pin);

  if (mode == OUTPUT) {
    return pinModeBit.write(0); // Set bit to 0 for output
  } else {
    return pinModeBit.write(1); // Set bit to 1 for input
  }
}

/**
 * @brief Set the state of a pin (HIGH/LOW).
 *
 * @param pin The pin number (0-7) to set.
 * @param val The state to set the pin (true for HIGH, false for LOW).
 * @returns Whether we successfully wrote the bit to the register.
 */
bool Adafruit_XCA9554::digitalWrite(uint8_t pin, bool val) {
  if (!i2c_dev || !output_port_reg || pin > 7) {
    return false; // Validate parameters
  }

  // Create a RegisterBits object for the specific pin
  Adafruit_BusIO_RegisterBits outputBit =
      Adafruit_BusIO_RegisterBits(output_port_reg, 1, pin);

  return outputBit.write(val ? 1 : 0); // Set the pin state
}

/**
 * @brief Read the state of a pin (HIGH/LOW).
 *
 * @param pin The pin number (0-7) to read.
 * @returns The state of the pin (true for HIGH, false for LOW), or false if an
 * error occurs.
 */
bool Adafruit_XCA9554::digitalRead(uint8_t pin) {
  if (!i2c_dev || !input_port_reg || pin > 7) {
    return false; // Validate parameters
  }

  // Create a RegisterBits object for the specific pin
  Adafruit_BusIO_RegisterBits inputBit =
      Adafruit_BusIO_RegisterBits(input_port_reg, 1, pin);

  uint8_t bitValue;
  bitValue = inputBit.read();

  return bitValue != 0; // Return true if bit is high, false if low
}
