#pragma once

// Bring up the local TFT and LVGL status screen. No-op if the TFT is disabled
// in menuconfig. Read-only view of device state; never a control path.
void ui_init(void);

// Mark that HID injection just happened; the UI flashes an ACTIVE banner for a
// short window afterwards. Safe to call from any task; cheap when TFT disabled.
void ui_note_activity(void);
