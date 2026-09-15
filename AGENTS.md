# PourBot release workflow

- Firmware versions use three numeric segments: major.minor.patch (for example, 1.1.1).
- For each requested software change, increment the patch version by one unless the user explicitly requests a major or minor revision.
- Update the version in the root CMakeLists.txt, build and verify the change, commit and push to https://github.com/kalendor/Pourbot-2, and publish the matching vMAJOR.MINOR.PATCH tag.
- Verify that the GitHub release workflow succeeds and publishes firmware.bin for OTA.
- Do not flash the scale over USB. The user installs and tests releases through OTA. USB flashing requires a new explicit user request.
- Preserve existing user changes. The enlarged OTA button is included in release 1.1.1.
