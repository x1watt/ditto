# Ditto architecture

Ditto is an ESP32-S3 that plugs into a target computer over USB and is driven
from the LAN. One device, three roles: USB HID (keyboard+mouse) and later USB
mass storage to the target; HDMI capture of the target's screen; and a
token-authenticated HTTP/WebSocket control API.

## CORE vs feature boundary

`CORE` modules own the USB device stack, the HID report path and the config/token.
A feature (capture, file exchange, UI) never reaches around them to poke USB
endpoints or rewrite descriptors directly; it calls the CORE APIs.

- **CORE**
  - `usb_descriptors.*` - the single source of truth for the USB device:
    descriptors, enumeration state, TinyUSB callbacks. Nothing else defines a
    USB descriptor or endpoint.
  - `hid_engine.*` - the only path that emits HID reports. Serializes all senders
    on one mutex so reports never interleave. Owns the key layout table.
  - `app_config.*` - NVS-backed config and the API bearer token.
- **Feature layer**
  - `net_api.*` - Wi-Fi, mDNS, HTTP API. Translates requests into CORE calls.
    Holds no USB knowledge beyond `usb_device_mounted()`.
  - `capture/*` (milestone 5) - HDMI receiver -> LCD_CAM -> JPEG ring. Produces
    frames; never touches USB.
  - `ui/*` (milestone 4) - LVGL on the local TFT. Read-only view of state plus the
    safety "ACTIVE" banner; never a control path.

## Key hardware constraint

The S3 has one USB OTG controller. It is a USB **device** to the target, so it
cannot also be a USB **host**. HDMI capture therefore must not use a USB UVC
dongle; it comes in through an HDMI-receiver chip (TC358743) on the LCD_CAM
parallel bus. If that bring-up stalls, the fallback is a second S3 in host mode,
joined to the same API over the LAN.

## Concurrency / task layout

- TinyUSB runs its own task (installed by `tinyusb_driver_install`).
- `esp_http_server` runs its worker task(s); HID commands execute inline there.
  Command strings are short, so this is acceptable; if a future feature needs
  long-running typing, move it to a dedicated queue+task behind `hid_engine`.
- Capture DMA + JPEG encode (milestone 5) run on their own task and must never
  run on whatever task drives LVGL (see performance.md).

## Data that must not be shared unguarded

The USB MSC backing store (milestone 2) and the LAN file API must not write the
same filesystem image concurrently. Access is serialized through an explicit
eject/attach handoff, never concurrent mounts.
