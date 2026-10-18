# Ditto

ESP32-S3 that plugs into a target computer over USB and is driven from the LAN,
so Claude on one machine can control other machines at home. It emulates a USB
keyboard + mouse (and, in later milestones, a USB drive and HDMI screen capture)
and exposes a token-authenticated HTTP API.

See `docs/architecture.md` and `docs/performance.md` for the design and its rules.

## Status

All seven milestones are implemented:

- USB composite **HID keyboard + mouse + mass storage** to the target (TinyUSB).
- MSC backing store is the internal SPI-flash `storage` partition by default, or a
  **microSD** card (set backend + slot pins in `menuconfig`).
- Wi-Fi STA + **mDNS** (`ditto.local`) + **HTTP control API** with bearer-token auth.
- Local **TFT status screen** (ST7789 over SPI, LVGL): hostname/IP, USB + disk
  state, API token, and a loud **HID ACTIVE** banner during injection. Pins and
  panel size in `menuconfig` (Ditto -> Local TFT); disable it there if headless.

- **HDMI capture** (off by default; enable in `menuconfig` -> Ditto -> HDMI capture):
  HDMI receiver (TC358743) parallel output -> S3 LCD_CAM (DVP) -> RGB565 -> software
  JPEG -> `GET /screenshot`. Low-res/low-fps by design (no HW JPEG on S3). The DVP +
  JPEG pipeline is generic (works with any parallel source for bring-up); the
  receiver register setup in `capture.c::tc358743_configure()` is a board-specific
  stub you must fill for your input timing. Capture failure never stops HID/API.

- **Live stream**: `GET /stream` is a WebSocket that pushes the latest JPEG as
  binary frames, capped at 5 fps, to up to 4 viewers. Auth on the handshake via the
  bearer header or a `?token=` query param. Dead clients are pruned automatically.
  Needs capture enabled; otherwise it connects but sends nothing.

- **File exchange** (M7): `POST /msc` hands the USB disk between host and firmware
  (`{"action":"eject"}` = firmware takes it so it can write; `"attach"` = host gets
  it back). While the firmware owns it, `POST /file?path=NAME` writes a file,
  `GET /file?path=NAME` reads it, `GET /files` lists them. The `/file*` routes
  refuse with 409 while the host owns the disk, which is what prevents both sides
  writing the shared FAT at once. Filenames are basenames only (no paths / `..`).

## Build & flash

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3            # first time only
idf.py menuconfig                    # set Ditto -> Home Wi-Fi SSID/password
idf.py build flash monitor
```

The USB-device signals are on the S3 internal PHY (GPIO19/20): flash over the
board's UART/JTAG USB port, and connect the **OTG** USB port to the target.

On first boot the firmware generates a random API token and logs it:

```
W (xxx) ditto: API token: 3f9a...   <-- copy this; it is stable across reboots
```

## API

All requests need `Authorization: Bearer <token>`. Base URL `http://ditto.local`
(or the IP from `/status`).

| Method | Path       | Body                                              |
|--------|------------|---------------------------------------------------|
| GET    | `/status`  | -                                                 |
| POST   | `/type`    | `{"text":"hello","delayMs":8}`                    |
| POST   | `/key`     | `{"keys":["ctrl","alt","delete"]}`                |
| POST   | `/mouse`   | `{"dx":10,"dy":-5}` / `{"scroll":{"v":-1}}` / `{"click":"left"}` / `{"button":"left","action":"down"}` |
| GET    | `/screenshot` | - (returns `image/jpeg`; 501 if capture off, 503 if no frame yet) |
| GET    | `/stream`  | WebSocket, pushes JPEG binary frames (`?token=` or bearer header) |
| POST   | `/msc`     | `{"action":"eject"}` (firmware takes disk) / `{"action":"attach"}` (host) |
| POST   | `/file`    | `?path=NAME`, body = file bytes (needs disk ejected to firmware) |
| GET    | `/file`    | `?path=NAME` → file bytes |
| GET    | `/files`   | - → JSON `[{"name","size"}]` |

`/key` names: modifiers `ctrl shift alt gui/win`; keys `enter esc tab space
backspace delete home end pageup pagedown up down left right f1..f12 capslock
printscreen`, plus any single printable character.

### Examples

```sh
TOK=3f9a...                       # from the boot log
H="Authorization: Bearer $TOK"
B=http://ditto.local

curl -s -H "$H" $B/status
curl -s -H "$H" -d '{"text":"echo hi\n"}'        $B/type
curl -s -H "$H" -d '{"keys":["ctrl","alt","delete"]}' $B/key
curl -s -H "$H" -d '{"click":"left"}'            $B/mouse
```

### Control loop (what Claude runs here)

`scripts/ditto.sh` wraps the API. Once HDMI capture lands (M5), the loop is:
screenshot -> decide -> type/click -> repeat. Until then it is blind HID control.

## Security

This device injects keystrokes into whatever it is plugged into. Treat the token
as a root credential. The HTTP server is LAN-only; never port-forward it. Anyone
on the LAN with the token has full keyboard/mouse control of the target.
