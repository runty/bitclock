# bitclock firmware

## Bootstrap

### One-time esp-idf setup

[esp-idf](https://github.com/espressif/esp-idf) (the ESP32 development framework) needs to be installed separately before building the firmware.

Bash shell

```fish
brew install cmake ninja dfu-util python@3.12
brew reinstall openssl
git clone -b v5.3.4 --recursive https://github.com/espressif/esp-idf.git
./install.sh esp32s3
source export.sh
```

Fish shell

```fish
brew install cmake ninja dfu-util python@3.12
brew reinstall openssl
git clone -b v5.3.4 --recursive https://github.com/espressif/esp-idf.git
./install.fish esp32s3
source export.fish
```

### Entering esp-idf environment

Bash shell

```sh
source ~/path-to-esp-idf/export.sh
```

Fish shell

```fish
source ~/path-to-esp-idf/export.fish
```

## Target directories

In order to maintain older PCB revision targets, we have separate directories for each PCB revision. Despite most files being shared across targets (via symlinks) we do this because esp-idf doesn't handle multiple targets well. This gives each target it's own build configuration and directory.

```fish
cd target-rev2-esp32s3/
```

To avoid confusing your code editor with duplicate files across targets, it is recommended to open VSCode with a single target as the root directory.

```fish
code .
```

## Build

```fish
idf.py build
```

## Flash

```fish
idf.py flash
```

## Monitor

```fish
idf.py monitor
```

## Debug

```fish
idf.py openocd
```

Then go to VSCode and "Start Debugging" (F5)

## Developer tips

### Bluetooth GATT attributes

Browsers and operating systems can cache GATT attributes by device identity and
service UUID. If you edit the attribute table for an existing service, the web
configurator may keep seeing the old service layout even after reflashing.

To resolve local cache issues during development, go into Bluetooth settings and
"Forget Device", then re-pair it to refresh the GATT cache.

For released changes, prefer one of these approaches:

- Preserve existing characteristic ordering in the original Bitclock service.
- Add new features to a new service UUID.
- Change the BLE identity address when a breaking GATT table change must be
  discovered by already-paired clients.

The firmware uses a static-random BLE identity address so central devices can
cache bonds and GATT services. That address is generated once per physical
device, stored in NVS under `ble_addr`, and reused on every boot. If NVS is
erased, the next boot generates a new valid static-random address. Devices
upgrading from firmware without `ble_addr` also generate this address on the
first boot after flashing, so users may need to forget and re-pair once after
that update.

Home Assistant MQTT settings are exposed through a separate service UUID,
`1f44d9a0-4f71-4c9d-9ce8-5f1a950cc1dd`, and the firmware currently advertises
that service so Web Bluetooth discovers a fresh service table. The MQTT config
characteristic UUID is `55940870-984a-4bbf-ae64-27b492ebd17e`.
