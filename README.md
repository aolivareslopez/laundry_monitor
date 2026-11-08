# Laundry Monitor

ESP32 firmware project built with PlatformIO. The current firmware is a hardware
bring-up test: it checks the LEDs, buzzer, OLED, I2C bus, servo, buttons, and
potentiometer. It does not yet implement laundry-cycle monitoring.

## Requirements

- ESP32 Dev Module (`esp32dev`)
- VS Code with the PlatformIO IDE extension, or the PlatformIO Core CLI
- The components listed in the wiring table below

PlatformIO installs the Adafruit SSD1306 and GFX libraries declared in
[`platformio.ini`](platformio.ini).

## Build and upload

Open this folder as a PlatformIO project, then use **Build** and **Upload** from
the PlatformIO toolbar. With PlatformIO Core installed, the equivalent commands
are:

```sh
pio run
pio run --target upload
pio device monitor --baud 115200
```

After uploading, open the serial monitor at **115200 baud**. On startup, the
firmware runs the hardware checks and scans the I2C bus. It then reports the
potentiometer and button states every 250 ms.

## Wiring

Connect the components to the ESP32 using this pin map:

| Component | ESP32 pin(s) | Notes |
|---|---|---|
| Red, yellow, green LEDs | 2, 4, 15 | Use appropriate current-limiting resistors |
| Buttons 1 and 2 | 32, 33 | Each button connects its pin to GND; firmware uses `INPUT_PULLUP` |
| Potentiometer | 34 | Wiper to GPIO 34; connect the other terminals to suitable supply and GND |
| Buzzer | 27 | Firmware beeps at 1 kHz during startup |
| Servo signal | 13 | Power the servo appropriately; share ground with the ESP32 |
| SPI OLED: CS, DC, reset | 5, 16, 17 | SCK = 18, MOSI = 23; OLED is configured as 128x64 |
| I2C bus: SDA, SCL | 21, 22 | Scanner prints detected addresses; expected kit devices are 0x60, 0x68, and 0x76 |

Check component voltage requirements and your board's pinout before connecting
hardware. Do not power a servo directly from an ESP32 GPIO.
