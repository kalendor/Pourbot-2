# PourBot _Touch_

**PourBot is a touchscreen coffee scale built to make pour-over brewing easier to follow, repeat, and improve.**

It combines live weight, brew time, flow rate, editable recipes, and saved brew graphs in one compact device. The interface is designed for the counter rather than the workbench: large numbers, simple controls, and useful guidance without unnecessary clutter.

[Visit the PourBot website](https://kalendor.github.io/Pourbot-2/) · [Download the latest release](https://github.com/kalendor/Pourbot-2/releases/latest)

## What PourBot does

- Works as a responsive everyday coffee scale before a brew begins.
- Tares automatically when a new brew starts.
- Displays live weight, elapsed time, and flow rate while pouring.
- Uses color to show whether the pour is slow, on target, or too fast.
- Includes editable V60 and Chemex-style recipes.
- Remembers the selected recipe and calibration after a restart.
- Draws a live graph of weight and flow during the brew.
- Saves the last 25 brews to a microSD card for later review.
- Connects to Wi-Fi for accurate date and time.
- Installs new firmware directly from the touchscreen through secure OTA updates.
- Enters deep standby to reduce battery use and wakes with a dedicated button.

## The main screens

### Scale and brew screen

The home screen keeps the current weight large and easy to read. Once a brew starts, PourBot adds the timer, recipe information, target progress, and flow guidance. Pausing does not tare the scale, and holding the Start button resets the brew and tares again.

### Live graph

The graph tracks weight and flow throughout the pour. Its time and weight ranges grow with the brew, so it can continue beyond a recipe's expected target or duration.

### Recipes

Recipes store the coffee dose, water ratio, bloom hold, time between pours, and number of pours. They can be edited from the touchscreen, and **Save & Use** immediately returns to the brewing screen.

### Analytics

PourBot stores up to 25 brew files on the microSD card. Each saved brew includes its graph, final weight, average flow, duration, and recipe information. The final weight is the highest weight reached during the brew, so removing the brewer before reset does not turn the result into zero.

## Parts list

| Part | What it does |
| --- | --- |
| **Guition JC3248W535C** | The ESP32-S3 controller, 3.5-inch touchscreen, USB connection, battery input, and built-in charging hardware. |
| **3–5 kg load cell** | Sits beneath the weighing platform and bends by a tiny amount as weight is added. |
| **HX711 board** | Converts the load cell's very small signal into stable weight readings PourBot can use. |
| **microSD card** | Stores the most recent brew graphs and statistics. A FAT32-formatted card works best. |
| **1000 mAh LiPo battery** | Powers PourBot away from USB. It connects through the display board's battery socket. |
| **Optional MAX17048 gauge** | Estimates the battery's remaining charge and reports it to PourBot. |
| **Wake button** | A normally-open momentary button used to wake PourBot from deep standby. |
| **Enclosure and platform** | Hold the electronics and transfer the brewer's weight cleanly to the load cell. |

## Basic wiring

The display board and HX711 use a shared 3.3 V supply and ground.

| Connection | PourBot pin |
| --- | --- |
| HX711 data / DOUT | GPIO17 |
| HX711 clock / SCK | GPIO18 |
| Standby wake button | GPIO6 to GND |
| MAX17048 SDA | GPIO7 |
| MAX17048 SCL | GPIO15 |
| SD card CS | GPIO10 |
| SD card MOSI | GPIO11 |
| SD card clock | GPIO12 |
| SD card MISO | GPIO13 |

> [!CAUTION]
> Battery connector polarity is not universal. Confirm the positive and negative pins on both the battery and board before connecting them. A reversed LiPo connection can damage the board or battery.

## Using PourBot

1. Turn on PourBot with the platform empty.
2. Wait for the startup logo to finish; the scale tares before the main screen appears.
3. Place the brewer on the scale and tap **Start**. PourBot tares the brewer and starts the timer.
4. Tap the same button to pause or resume without taring.
5. Hold the button to finish and reset. The brew is saved to the SD card when recording data is available.

Swipe between the two main pages. Use the menu for recipes, analytics, calibration, Wi-Fi, updates, and other settings. Swipe right from the home page to reach reboot and standby controls.

## Calibration

Calibration only takes a known weight:

1. Remove everything from the platform and tap **Tare Empty**.
2. Place a known weight on the scale.
3. Tap **Touch to Enter** and enter the weight in grams.
4. Tap **Calibrate** and wait for the button to turn green.

The result is saved automatically and remains available after rebooting.

## Installing PourBot

The easiest first installation uses the browser installer on the [PourBot website](https://kalendor.github.io/Pourbot-2/). Use desktop Chrome or Edge and a USB data cable.

The browser installer performs a complete installation and clears saved calibration, recipes, and Wi-Fi settings. Once PourBot is installed, normal updates can be applied from **Menu → Settings → OTA Update** without erasing those settings.

Keep PourBot connected to USB power while installing an update.

## Building from source

This project uses PlatformIO and the `LVGL-320-480` environment. ESP-IDF requires a build path without spaces, so copy the project to a simple temporary path before building:

```powershell
pio run -d C:\Temp\pourbot-build
```

USB flashing is intended for the first installation or recovery. Routine releases are delivered through PourBot's OTA update screen.

For deeper implementation details, see:

- [Analytics and storage notes](ANALYTICS.md)
- [Main application and scale controls](src/DEMO_LVGL.c)
- [Recipe definitions](src/recipes.c)
- [Battery gauge](src/battery_gauge.c)
- [Display configuration](src/display.h)

## Project status

PourBot is a personal open-source project and continues to evolve through real brewing and hardware testing. Suggestions, bug reports, and improvements are welcome through [GitHub Issues](https://github.com/kalendor/Pourbot-2/issues).

## License

PourBot is available under the [MIT License](LICENSE).
