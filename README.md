# PourBot Touch

ESP-IDF firmware for the Guition JC3248W535C ESP32-S3 display and HX711 scale. LVGL 8.4 drives the 480x320 landscape touchscreen using software rotation.

## Wiring

- HX711 DOUT: GPIO17; SCK: GPIO18; supply: 3.3V and GND.
- Standby wake button: normally-open momentary button between GPIO6 and GND.
- Battery power and charging use the board hardware. Optional Adafruit MAX17048 #5580: SDA GPIO7, SCL GPIO15, VIN regulated 3.3V, GND common ground. Its battery sockets connect the battery and board battery input with verified polarity/adapters. No MCU battery ADC or web server runs.

The MAX17048 uses dedicated I2C controller 1 at address 0x36. A low-priority task reads it every five seconds, independently of HX711 sampling. Battery icons show gray `--%` when unavailable, green normally, amber at 11–20%, and red at 0–10%. The gauge needs a connected battery to respond. It reports estimated state of charge, not confirmation that the charger is active. It is not reset on each reboot, preserving its ongoing battery estimate.

## Operation

The splash remains visible during startup tare. Swipe left for the dynamic weight/flow chart and recipes; swipe right from the main screen for reboot and standby.

Start automatically tares; pause/resume preserves weight. Holding Start resets the brew, clears the chart, and tares. Standby enters deep sleep; GPIO6 wakes into a fresh boot.

Settings opens touchscreen calibration: tare empty, enter a known mass, then calibrate. Successful buttons turn green. Calibration and the last selected recipe persist in NVS. There are no serial calibration commands.

## Build

Analytics stores chart snapshots on a FAT32 SD card and browses the latest 25 pours. Settings includes Wi-Fi setup for internet time and secure OTA updates. See [ANALYTICS.md](ANALYTICS.md) for save behavior, CSV format, and offline timestamps.

Use PlatformIO environment `LVGL-320-480`. ESP-IDF requires a space-free build path: copy the project to a temporary folder without spaces, excluding `.pio`, then run:

```powershell
pio run -d C:\Temp\pourbot-build
pio run -d C:\Temp\pourbot-build -t upload --upload-port COM9
```

Check the actual USB port before uploading. Normal uploads preserve NVS; do not erase flash unless saved data should be deleted.

## OTA updates

The first OTA-capable build must be flashed over USB because it installs the
factory/OTA partition table and rollback-enabled bootloader. After that:

1. Increase the version in the root `CMakeLists.txt`.
2. Commit and push the change.
3. Tag the commit (for example, `v1.2.0`) and push the tag.
4. GitHub Actions builds a release and publishes `firmware.bin`.
5. On PourBot, open **Menu > Settings > OTA Update**, tap **Check for Updates**,
   then tap the green **Update PourBot** button.

Keep PourBot connected to USB power during an update. Downloads use HTTPS and
the device retains the previous application image for automatic rollback.

Application logic and scale pins: `src/DEMO_LVGL.c`. Recipes: `src/recipes.c`. Display pins: `src/display.h`. LVGL configuration: `src/lv_conf.h`. Keep the numeric font uncompressed (`bitmap_format = 0`).
