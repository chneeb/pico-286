#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "inttypes.h"
#include "stdbool.h"

// ─── PicoCalc keyboard (ClockworkPi STM32 keyboard MCU over I2C) ────────────
// The MCU also owns both backlights and the battery gauge.

#ifndef PICOCALC_KBD_I2C
#define PICOCALC_KBD_I2C i2c1
#endif
#ifndef PICOCALC_KBD_SDA_PIN
#define PICOCALC_KBD_SDA_PIN (6)
#endif
#ifndef PICOCALC_KBD_SCL_PIN
#define PICOCALC_KBD_SCL_PIN (7)
#endif
#ifndef PICOCALC_KBD_ADDR
#define PICOCALC_KBD_ADDR (0x1F)
#endif

// The ClockworkPi header's 10 kHz applies to sharing the bus with a second
// device. Nothing else is on i2c1 here, and shapones drives this same keyboard
// at 400 kHz on this same board.
#ifndef PICOCALC_KBD_HZ
#define PICOCALC_KBD_HZ (400000)
#endif

// Half of one poll cycle. The MCU needs time between the register-select write
// and the read, so a full cycle is two of these - which lands on the MCU's own
// 16 ms matrix scan period (KEY_POLL_TIME).
#ifndef PICOCALC_KBD_POLL_US
#define PICOCALC_KBD_POLL_US (8000)
#endif

// Matches the PS/2 driver's entry point so pico-main.c does not have to care
// which keyboard is attached.
void keyboard_init(void);

// Drains the keyboard MCU's event FIFO and feeds XT scancodes to
// handleScancode(). Safe to call as often as you like; it rate-limits itself.
void picocalc_kbd_poll(void);

// Scancodes are queued rather than written straight to port 0x60, which has no
// hardware queue: two codes emitted without the emulated CPU running in between
// would collapse to the last one. Call picocalc_kbd_pump() to release one, and
// give the emulation a slice while picocalc_kbd_pending() is true.
bool picocalc_kbd_pending(void);
void picocalc_kbd_pump(void);

int picocalc_read_battery(void);

// Keyboard backlight (0..255). The panel backlight is picocalc_lcd_set_backlight().
void picocalc_kbd_set_backlight(uint8_t level);

// ─── Mouse emulation ───────────────────────────────────────────────────────
// The PicoCalc has no pointing device, and pico-286 emulates a mouse only as
// hardware (a Microsoft serial mouse on COM1) - there is no INT 33h in the
// emulator, so a DOS mouse driver such as CTMOUSE still has to be loaded for
// software to see one. This just supplies the movement that driver reads.
//
// Ctrl-Alt-M toggles mouse mode. While it is on, the four arrow buttons drive
// the pointer instead of emitting scancodes, and [ and ] are the left and right
// buttons. The keyboard backlight comes on as an indicator, since a normal
// build has no other status channel.
//
// Call once per pass of the emulation loop. Returns true when a packet should
// be sent, i.e. pass the values straight to sermouseevent(). Rate-limits
// itself, and derives motion from which arrows are currently held rather than
// from key events - the I2C poll only drains one event per cycle, so
// event-driven motion would be lumpy.
// True while Ctrl-Alt-M mouse mode is active. The keyboard backlight is the
// other indicator, which is useless if the backlight is turned down.
bool picocalc_mouse_mode(void);

bool picocalc_mouse_step(uint8_t *buttons, int8_t *dx, int8_t *dy);

#ifdef __cplusplus
}
#endif
