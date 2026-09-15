# Firmware cleanup — September 14, 2026

Reviewed application state, UI creation/update paths, HX711 acquisition and filtering, recipe persistence, calibration, standby/wake, display/touch synchronization, and build inputs.

## Removed

- Archived Wi-Fi screens/remote controls and four excluded Wi-Fi/web source files.
- Disabled battery ADC setup, sampling, gauge widgets, and test switches. GPIO6 remains the standby wake input.
- Unused legacy dashboard and removed pour-guidance page, including stage/hold state and update calls.
- Hidden summary, recipe-detail, calibration-raw, and calibration-factor widgets and their repeated formatting work.
- Write-only display telemetry, obsolete chart-freeze flag, unused recipe save/select APIs, and temporary ten-minute serial scale diagnostics.
- Disabled hardware-rotation experiment and duplicate GPIO include.
- Enabled-but-unused LVGL widget, stress, and music demos.

CMake now explicitly lists application C files. Application logging retains warnings/errors. Debug build mode and display timing are unchanged to avoid retuning a working device. README reflects the current touchscreen workflow rather than obsolete serial commands.

## Preserved deliberately

- HX711 timing, median/movement filtering, display zero hysteresis, and tare/calibration sample counts.
- Boot tare under the splash, scale/brew layouts, dynamic chart history, recipe editing and NVS format.
- LVGL/display synchronization tasks, touch processing, deep sleep and GPIO6 wake.
- Vendor library/driver APIs: externally visible support functions are not assumed unused merely because the application does not call every one.
- Analytics menu placeholder: still an existing visible entry, not implemented analytics.
- Flash partitions and existing NVS data. Removing networking code does not erase previously stored Wi-Fi credentials.

## Follow-up considerations

Recipe and calibration persistence currently do not report commit failures to the UI. Saved recipe blobs are not range-validated, and calibration factors are not checked for non-finite values. These deserve a separate persistence-hardening change rather than changing storage behavior during dead-code removal.

Shared brew/calibration state crosses LVGL and scale tasks; a future command queue could make ownership explicit. Display and sampling scheduling were not redesigned here. Deep sleep still cannot remove the board regulator/charger's own standby consumption.

## Validation

Fresh build and final upload succeeded on COM9 at 921600 baud; flash hashes verified. Final static RAM is 30,840 bytes and reported program size is 858,481 bytes. Linked-symbol checks found no Wi-Fi initialization, HTTP server startup, battery ADC read, legacy/guidance screen, or music/stress demo entry points. NVS at 0x9000–0xDFFF was outside the upload erase ranges.

Critical HX711 read/average, median, manual tare, brew tare, and brew-control functions were compared with the backup and are unchanged. Verify on hardware after flashing: startup zero, tare with weight added/removed, start/pause/resume/reset, recipe save/reboot persistence, calibration keypad/success colors, chart scaling, page swipes, standby and GPIO6 wake.

Pre-cleanup recovery copy: `C:\Users\Rob\AppData\Local\Temp\pourbot-before-cleanup` (temporary storage; copy elsewhere for long-term retention).
