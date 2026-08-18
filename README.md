# PlantHubOS

**Your plants, on your network — no cloud, no account, no app store.**

> **Heads-up:** `main` is the V2 development line and may be broken or
> incompatible at any commit. The latest stable firmware is always the
> newest [v1.x release](https://github.com/nebeleben/planthubos/releases).

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
it to an ESP32-C6 or ESP32-C5 board.

### Which download?

Each release ships three regional builds per chip:

- **eu** — Europe / UK / most of the world, WiFi channels 1–13.
- **us** — United States / Canada, WiFi channels 1–11.
- **jp** — Japan, WiFi channels 1–14.

Pick the one for where you'll actually use the hub — **using the wrong
region's build for your location is a regulatory violation.** You're not
locked in: the region is also changeable later from the hub's own Config
page, and takes effect on the next reboot.

Full flash (first install, ESP32-C6, EU build):

```
esptool --chip esp32c6 write_flash \
  0x0 bootloader-esp32c6.bin \
  0x8000 partition-table-esp32c6.bin \
  0x19000 ota_data_initial-esp32c6.bin \
  0x30000 planthub-esp32c6-eu.bin
```

(Swap `planthub-esp32c6-eu.bin` for the `-us`/`-jp` variant as needed —
`bootloader`/`partition-table`/`ota_data_initial` don't vary by region,
only by chip.)

**Do not reuse these offsets for the ESP32-C5.** It is still on the older
4 MB partition layout, and every offset in the command differs:

```
esptool --chip esp32c5 write_flash \
  0x2000 bootloader-esp32c5.bin \
  0x8000 partition-table-esp32c5.bin \
  0xf000 ota_data_initial-esp32c5.bin \
  0x20000 planthub-esp32c5-eu.bin
```

Then connect to the `PlantHub-XXXX` WiFi it broadcasts and follow the setup
page. Updates after that happen over the air from the hub's own web page.

## Troubleshooting

**WiFi first.** Almost every setup problem is one of these:

- **ESP32 is 2.4 GHz only.** The hub cannot see or join a 5 GHz-only
  network. If your router merges both bands under one name, that's fine —
  just make sure 2.4 GHz is enabled.
- **Hub joins WiFi, but you can't find its page**: it has no fixed name on
  your network yet — look up its IP in your router's device list (it shows
  up as `PlantHub-XXXX`).
- **A node never pairs**: usually channels. If your router sits on channel
  12 or 13 and you flashed the **us** build, the node is not allowed to
  transmit there and can never find the hub — use the right region build,
  or set the region on the hub's Config page and re-pair. Pairing also
  wants the node reasonably close to the hub for its first handshake.
- **The setup network (`PlantHub-XXXX`) doesn't appear** after first
  flash: power-cycle once; if it still doesn't, re-run the full first
  install (all four files) rather than just the app image.
- **Locked out or in a weird state?** Three ways back, mildest first:
  - **Can't reach the WiFi settings, but can reach the plug?** Unplug and
    replug power **5 times in a row** (each within ~10 seconds of boot):
    the WiFi settings are cleared and the setup network comes back.
    Everything else — plants, history, pairings — survives. Only real
    power cycles count, so normal reboots and updates can never trigger it.
    (On a *node*, the same gesture clears its role and pairing instead —
    that's a node's way back to the setup network.)
  - **Hold the BOOT button for 10 seconds**: factory reset — WiFi, claim
    and pairings are cleared, your plants and their history survive, and
    the setup network comes back.
  - **From the hub's own page** (claimed hubs only): the Config tab has
    both a *Factory reset* that keeps your plant data and a *Hard factory
    reset* that wipes the hub back to a fresh install.

## License

PlantHubOS is licensed under the [Apache License 2.0](LICENSE).
