# Pour history and network time

Analytics is available from the menu. It lists the newest 25 PourBot CSV files on the SD card. Selecting a file opens a separate, read-only amber-weight/blue-flow graph with dynamic axes, final weight, average flow, and recorded duration. Final weight is the highest weight reached during the recorded pour, so removing the brewer before reset does not make the report read zero. It does not replace or reset the live graph.

Hold START to reset a brew, enter standby, or tap SAVE CURRENT in Analytics to queue a snapshot of the recorded chart. Pause/resume alone does not save a file. Unchanged snapshots are not queued twice. A changed snapshot saved again is a separate file. A pour must have detected flow and at least one chart sample to be saved. The chart starts when flow is first detected, so its recorded duration excludes the initial wait before pouring. Chart history is bounded at 1,200 samples and progressively coarsened on very long sessions.

## Files

Files are stored under `/pours` on the card. Example: `9142026-19_29.csv` for September 14, 2026 at 7:29 PM local time. The timestamp represents brew start. Same-minute collisions receive `-01`, `-02`, etc. Offline pours without valid system time use `unsynced-<sequence>.csv`. They are not retrospectively renamed. A persistent sequence orders the browser independently of timestamp and filename spelling.

The first line is versioned metadata: `POURBOT,1,sequence,epoch,dose,target,sample_count,recipe`. Then `time_s,weight_g,flow_g_s` and one row per recorded sample. Weight inherits the live chart's whole-gram sampling; flow retains tenths. Older files remain on the card; only the latest 25 are shown. No automatic card formatting or deletion of old history is performed.

Use a FAT32 card, inserted before use. Missing, unreadable, full, or incompatible cards produce an Analytics error, not a firmware crash. An asynchronous save can fail; do not remove power until it finishes. If saving fails after a reset, the cleared live samples cannot be recovered. Standby waits for outstanding SD jobs and postpones sleep if they remain busy after five seconds.

SD SPI pins: CS GPIO10, MOSI GPIO11, MISO GPIO13, CLK GPIO12; controller SPI3. Display remains on SPI2. Archive buffers/jobs are in PSRAM. Files are written to an owned temporary file, flushed and closed, then renamed into the archive.

## Wi-Fi

Settings → WI-FI SETUP → SCAN → select network → enter password → CONNECT. CONNECT is hidden until a network is selected. BACK from password entry returns to the network list. Wi-Fi startup, scan, and connect run outside LVGL and HX711 acquisition. Failed connection attempts stop after bounded retries/30 seconds. Credentials use the existing `pourbot/wifi_ssid` and `wifi_pass` NVS keys and are committed after successful connection, with automatic connection on the next boot.

Only station Wi-Fi is enabled: no recovery hotspot and no web server. Modem power-saving is enabled, but Wi-Fi still adds power consumption. Internet SNTP sets time; the local timezone is US Eastern with daylight-saving transitions, appropriate for West Warwick. Wi-Fi is stopped before deep sleep. MAX17048 uses its independent I2C bus unchanged; ESP32 battery ADC sampling stays absent.

## Later OTA version

OTA is not implemented here. The existing flash table has one factory application slot and a large internal storage partition, not an A/B OTA pair. A later release must deliberately migrate to two application slots and implement authenticated update delivery, image validation, and rollback. This release leaves flash offsets and user NVS intact. No OTA upload endpoint is exposed.

## Hardware checks

Confirm card mount and a saved file after reset. Open that file from Analytics, test axes with a long/heavy pour, and check that the live chart is untouched. Save two pours within one minute and verify distinct filenames. Verify wrong-password recovery, scan responsiveness, saved Wi-Fi after reboot, local timestamps, offline naming, and standby/GPIO6 wake. Check scale stability with Wi-Fi connected. These require the actual card, network, and scale; compilation alone cannot verify them.
