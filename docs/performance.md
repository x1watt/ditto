# Ditto performance rules

These are the regressions to avoid, not aspirations.

## Nothing heavy on the UI task

LVGL (milestone 4) runs its own rendering tick. Never run JPEG encode, SD-card
I/O, capture DMA completion work or network blocking on the LVGL task. The UI
task only reads already-computed state (status struct, latest thumbnail pointer)
and draws. Hand it a downscaled thumbnail, not a full frame to resize.

## Capture pipeline stays bounded

- Capture into a fixed, small ring of frame buffers in PSRAM. Drop frames under
  backpressure; never grow an unbounded queue.
- JPEG-encode once per delivered frame, not per client. `/screenshot` and every
  `/stream` client share the latest encoded frame.
- Cap stream FPS in firmware. The TC358743 path is low-bandwidth; target a legible
  still/low-fps feed, not smooth video.

## HID path is allocation-free on the hot path

`hid_engine` report senders do no heap allocation and hold the mutex only for the
duration of a report pair. Typing delay is cooperative (`vTaskDelay`), so it never
spins a core.

## Network

- HTTP handlers read request bodies into fixed stack buffers with hard size caps
  (`/type` 2KB, `/key` 512B, `/mouse` 256B). No unbounded body reads.
- File transfer (milestone 7) streams to/from SD in chunks; it never buffers a
  whole file in RAM.

## PSRAM

Large buffers (capture frames, JPEG scratch, file chunks) live in PSRAM. Keep
DMA-capable internal RAM for descriptors and small hot buffers only.
