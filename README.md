# PlantHubOS

**Your plants, on your network — no cloud, no account, no app store.**

PlantHubOS is the open firmware: an ESP32 hub for Xiaomi MiFlora plant sensors. Plug it in,
join its setup WiFi once, and it quietly watches your plants for you:

- 🌱 **Plants first** — you track *plants*, not gadgets. Swap a broken
  sensor and the plant's history just continues.
- 📈 **30 days of history** on the device itself — moisture, temperature,
  light and fertility, viewable from any browser on your network.
- 📡 **Rooms out of reach?** Add extra ESP32 *nodes* that relay sensor
  readings back to the hub — battery-powered if you like, updated over the
  air from the hub.
- 🏠 **Home Assistant ready** — turn on MQTT and your plants appear in
  Home Assistant automatically. InfluxDB push included too.
- 🔌 **Works offline** — no internet needed, ever. The hub keeps collecting
  even when your WiFi is down.

## Get it

Grab the latest firmware from the
[Releases](https://github.com/nebeleben/planthubos/releases) page and flash
it to an ESP32 or ESP32-C3 board. Full flash (first install):

```
esptool --chip esp32c3 write_flash \
  0x0 bootloader-esp32c3.bin \
  0x8000 partition-table-esp32c3.bin \
  0xf000 ota_data_initial-esp32c3.bin \
  0x20000 planthub-esp32c3.bin
```

Then connect to the `PlantHub-XXXX` WiFi it broadcasts and follow the setup
page. Updates after that happen over the air from the hub's own web page.

## About this repository

This is the read-only source mirror of PlantHub's firmware, published from a
private development mono-repo. Issues are welcome here; the code itself is
force-pushed by the mirror pipeline, so direct pull requests cannot be
merged in place.

Home Assistant and HomeBridge integrations live in their own repositories.
