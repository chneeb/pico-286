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

int picocalc_read_battery(void);

#ifdef __cplusplus
}
#endif
