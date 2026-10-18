#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Mouse button bitmask (matches TinyUSB MOUSE_BUTTON_*).
#define DITTO_MB_LEFT   0x01
#define DITTO_MB_RIGHT  0x02
#define DITTO_MB_MIDDLE 0x04

void hid_engine_init(void);

// Type a UTF-8 string one key at a time (US layout). Non-ASCII bytes are
// skipped for now. delay_ms spaces keypresses (0 => a small default).
esp_err_t hid_type_text(const char *utf8, uint32_t delay_ms);

// Tap a chord: hold `modifiers` (bitmask of KEYBOARD_MODIFIER_*) + up to
// 6 keycodes, then release. Used for Ctrl-Alt-Del, shortcuts, etc.
esp_err_t hid_key_tap(uint8_t modifiers, const uint8_t *keycodes, size_t n);

// Resolve a key name ("ctrl","enter","f4","a",...) to a modifier bit or a
// keycode. Exactly one of *modifier / *keycode is set non-zero on success.
esp_err_t hid_keyname_lookup(const char *name, uint8_t *modifier, uint8_t *keycode);

esp_err_t hid_mouse_move(int8_t dx, int8_t dy);
esp_err_t hid_mouse_scroll(int8_t vertical, int8_t horizontal);
esp_err_t hid_mouse_buttons(uint8_t buttons);   // hold this button bitmask
esp_err_t hid_mouse_click(uint8_t button);       // press + release once
