"""Preserve released firmware when publishing documentation-only site changes."""
import json
import os
from pathlib import Path
from urllib.request import Request, urlopen

headers = {'User-Agent': 'PourBot-site-builder'}
if os.environ.get('GH_TOKEN'):
    headers['Authorization'] = 'Bearer ' + os.environ['GH_TOKEN']
with urlopen(Request('https://api.github.com/repos/kalendor/Pourbot-2/releases/latest', headers=headers), timeout=30) as response:
    release = json.load(response)
version = release['tag_name'].removeprefix('v')
assert len(version.split('.')) == 3 and all(p.isdigit() for p in version.split('.'))
destination = Path(__file__).resolve().parents[1] / 'site/firmware'
destination.mkdir(exist_ok=True)
assets = {a['name']: a for a in release['assets']}
for name in ('firmware.bin', f'pourbot-{version}.bin'):
    url = assets[name]['browser_download_url']
    assert url.startswith('https://github.com/kalendor/Pourbot-2/releases/download/')
    # Do not forward API credentials to public asset downloads.
    with urlopen(Request(url, headers={'User-Agent': 'PourBot-site-builder'}), timeout=90) as response:
        data = response.read()
    assert len(data) == assets[name]['size'] and data and data[0] == 0xE9
    (destination / name).write_bytes(data)
manifest = {'name': 'PourBot', 'version': version, 'new_install_prompt_erase': True,
            'new_install_improv_wait_time': 0,
            'builds': [{'chipFamily': 'ESP32-S3', 'parts': [{'path': f'pourbot-{version}.bin', 'offset': 0}]}]}
(destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(f'Restored PourBot {version} installer and OTA assets')
