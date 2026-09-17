"""Package the matching bootloader, partitions and application for web installation."""
import json
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
build = root / '.pio/build/LVGL-320-480'
version = sys.argv[1].removeprefix('v')
destination = root / 'site/firmware'
destination.mkdir(parents=True, exist_ok=True)
# The application-only image is also the stable, redirect-free OTA endpoint.
shutil.copy2(build / 'firmware.bin', destination / 'firmware.bin')
# PlatformIO's flash arguments supply exact offsets and board flash settings.
args = json.loads((build / 'flasher_args.json').read_text())
esptool = pathlib.Path.home() / '.platformio/packages/tool-esptoolpy/esptool.py'
output = destination / f'pourbot-{version}.bin'
command = [sys.executable, str(esptool), '--chip', 'esp32s3', 'merge_bin', '-o', str(output)]
for name, value in args['flash_settings'].items():
    command += ['--' + name, str(value)]
for offset, filename in [('0x0', 'bootloader.bin'), ('0x8000', 'partitions.bin'),
                         ('0x10000', 'firmware.bin')]:
    command += [offset, str(build / filename)]
subprocess.run(command, check=True)
manifest = {'name': 'PourBot', 'version': version, 'new_install_prompt_erase': True,
            'new_install_improv_wait_time': 0,
            'builds': [{'chipFamily': 'ESP32-S3', 'parts': [{'path': output.name, 'offset': 0}]}]}
(destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
