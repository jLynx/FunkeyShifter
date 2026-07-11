# Toolchain Notes

This project uses PlatformIO with ESP-IDF, not Arduino.

## PlatformIO

Use the PlatformIO extension in VS Code for the normal Build, Upload, and
Monitor actions.

The first build installs the ESP-IDF framework, compiler toolchains, CMake,
Ninja, Python dependencies, and ESP-IDF components. That is expected.

## ESP-IDF Components

ESP-IDF components are declared in `src/idf_component.yml`:

```yaml
dependencies:
  espressif/esp_tinyusb:
    version: "^2.0.1~1"
```

The ESP-IDF component manager downloads those dependencies into
`managed_components/`. That folder is generated build state and should not be
committed. Keep `src/idf_component.yml` and `dependencies.lock` in git so the
dependency request and resolved versions are reproducible.

## Generated Files

These paths are ignored by git:

```text
.pio/
managed_components/
sdkconfig.*
```

`sdkconfig.defaults` is not ignored. It is the committed ESP-IDF configuration
seed for clean builds.

## Flash Size

The project is configured for an ESP32-S3 Super Mini. PlatformIO does not ship a
dedicated Super Mini board profile here, so the environment uses
`esp32-s3-devkitm-1` as the closest ESP-IDF S3 base board and overrides flash
size settings for the Super Mini:

```ini
board = esp32-s3-devkitm-1
board_build.flash_mode = qio
board_upload.flash_size = 4MB
board_upload.maximum_size = 4194304
```

```text
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
```

If PlatformIO reports a flash size mismatch, change both `platformio.ini` and
`sdkconfig.defaults` to the same value, then rebuild.

## Common Fixes

If the ESP-IDF Python virtual environment becomes corrupt, remove or rename the
ESP-IDF virtual environment under your PlatformIO home directory. The next build
recreates it.

If a clean build fails on Windows because CMake scratch files are locked, run
Build again from the PlatformIO extension. In this repo, a second run
regenerated the partial CMake state successfully.
