"""Check the version in the ESP application descriptor before publishing."""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
expected = re.search(r'set\(PROJECT_VER "(\d+\.\d+\.\d+)"\)',
                     (root / 'CMakeLists.txt').read_text()).group(1)
image = pathlib.Path(sys.argv[1]).read_bytes()
# ESP image header (24), first segment header (8), app descriptor version (16).
assert image[32:36] == bytes.fromhex('3254cdab'), 'Missing ESP app descriptor'
actual = image[48:80].split(b'\0', 1)[0].decode('ascii')
assert actual == expected, f'Embedded version {actual!r} != release {expected!r}'
if len(sys.argv) > 2:
    assert sys.argv[2] == f'v{expected}', 'Release tag does not match firmware'
print(f'Firmware version verified: {actual}')
