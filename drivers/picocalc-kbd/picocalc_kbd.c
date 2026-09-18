//
// PicoCalc keyboard -> XT (set 1) scancodes.
//
// The emulator wants what a PC/XT keyboard puts on port 0x60: a make code when
// a key goes down and make|0x80 when it comes up. The PicoCalc's keyboard MCU
// instead reports mostly-ASCII key codes with a press/hold/release state, and
// it has already folded Shift and Sym into the character (Shift+1 arrives as
// '!', not as "shift down" + "1"). So the mapping is:
//
//   * printable ASCII -> (scancode, needs_shift) lookup, synthesising a Shift
//     make/break pair around the key when the character requires one and no
//     physical Shift is already down.
//   * the real modifier keys (Ctrl, Alt, both Shifts) are reported as their own
//     events, so they pass through as ordinary make/break.
//   * everything else goes through a small special-key table.
//
// Physical Shift state is tracked separately so we never emit a Shift break
// while the user is still holding the key - that would desync the emulated
// keyboard for the rest of the session.

#include <stdbool.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include "printf.h"   // project printf -> DEBUG_VRAM, not a serial port

#include "picocalc_kbd.h"
#include "picocalc_lcd.h"

extern bool handleScancode(uint32_t ps2scancode);

// ─── Keyboard MCU protocol ─────────────────────────────────────────────────

#define REG_ID_BKL 0x05 // LCD backlight
#define REG_ID_FIF 0x09 // key event FIFO
#define REG_ID_BK2 0x0A // keyboard backlight
#define REG_ID_BAT 0x0B // battery
#define REG_WRITE_BIT 0x80

// Key codes, from PicoCalc/Code/picocalc_keyboard/keyboard.h
#define KEY_BACKSPACE 0x08
#define KEY_TAB       0x09
#define KEY_ENTER     0x0A
#define KEY_MOD_ALT   0xA1
#define KEY_MOD_SHL   0xA2
#define KEY_MOD_SHR   0xA3
#define KEY_MOD_SYM   0xA4
#define KEY_MOD_CTRL  0xA5
#define KEY_ESC       0xB1
#define KEY_LEFT      0xB4
#define KEY_UP        0xB5
#define KEY_DOWN      0xB6
#define KEY_RIGHT     0xB7
#define KEY_CAPS_LOCK 0xC1
#define KEY_BREAK     0xD0
#define KEY_INSERT    0xD1
#define KEY_HOME      0xD2
#define KEY_DEL       0xD4
#define KEY_END       0xD5
#define KEY_PAGE_UP   0xD6
#define KEY_PAGE_DOWN 0xD7
#define KEY_F1        0x81
#define KEY_F10       0x90

// Event states in the low byte of a FIFO entry
#define KEY_STATE_PRESSED  1
#define KEY_STATE_HOLD     2
#define KEY_STATE_RELEASED 3

// ─── XT set 1 scancodes ────────────────────────────────────────────────────

#define XT_ESC       0x01
#define XT_BACKSPACE 0x0E
#define XT_TAB       0x0F
#define XT_ENTER     0x1C
#define XT_CTRL      0x1D
#define XT_LSHIFT    0x2A
#define XT_RSHIFT    0x36
#define XT_ALT       0x38
#define XT_CAPS      0x3A
#define XT_BREAK     0x46

// XT keypad codes. This is an XT-class machine, so the plain (non-0xE0)
// keypad codes are the right ones for the cursor block - DOS software reads
// them directly and they need no extended-prefix handling in the emulator.
#define XT_KP_STAR  0x37
#define XT_HOME     0x47
#define XT_UP       0x48
#define XT_PGUP     0x49
#define XT_KP_MINUS 0x4A
#define XT_LEFT     0x4B
#define XT_RIGHT    0x4D
#define XT_KP_PLUS  0x4E
#define XT_END      0x4F
#define XT_DOWN     0x50
#define XT_PGDN     0x51
#define XT_INSERT   0x52
#define XT_DEL      0x53

#define NEEDS_SHIFT 0x80

// ASCII 0x20..0x7E -> XT make code, high bit set when the character is only
// reachable with Shift on a US layout.
static const uint8_t ascii_to_xt[95] = {
    /* ' ' */ 0x39, /* ! */ 0x02 | NEEDS_SHIFT, /* " */ 0x28 | NEEDS_SHIFT,
    /* # */ 0x04 | NEEDS_SHIFT, /* $ */ 0x05 | NEEDS_SHIFT, /* % */ 0x06 | NEEDS_SHIFT,
    /* & */ 0x08 | NEEDS_SHIFT, /* ' */ 0x28, /* ( */ 0x0A | NEEDS_SHIFT,
    /* ) */ 0x0B | NEEDS_SHIFT, /* * */ 0x09 | NEEDS_SHIFT, /* + */ 0x0D | NEEDS_SHIFT,
    /* , */ 0x33, /* - */ 0x0C, /* . */ 0x34, /* / */ 0x35,
    /* 0 */ 0x0B, /* 1 */ 0x02, /* 2 */ 0x03, /* 3 */ 0x04, /* 4 */ 0x05,
    /* 5 */ 0x06, /* 6 */ 0x07, /* 7 */ 0x08, /* 8 */ 0x09, /* 9 */ 0x0A,
    /* : */ 0x27 | NEEDS_SHIFT, /* ; */ 0x27, /* < */ 0x33 | NEEDS_SHIFT,
    /* = */ 0x0D, /* > */ 0x34 | NEEDS_SHIFT, /* ? */ 0x35 | NEEDS_SHIFT,
    /* @ */ 0x03 | NEEDS_SHIFT,
    /* A */ 0x1E | NEEDS_SHIFT, /* B */ 0x30 | NEEDS_SHIFT, /* C */ 0x2E | NEEDS_SHIFT,
    /* D */ 0x20 | NEEDS_SHIFT, /* E */ 0x12 | NEEDS_SHIFT, /* F */ 0x21 | NEEDS_SHIFT,
    /* G */ 0x22 | NEEDS_SHIFT, /* H */ 0x23 | NEEDS_SHIFT, /* I */ 0x17 | NEEDS_SHIFT,
    /* J */ 0x24 | NEEDS_SHIFT, /* K */ 0x25 | NEEDS_SHIFT, /* L */ 0x26 | NEEDS_SHIFT,
    /* M */ 0x32 | NEEDS_SHIFT, /* N */ 0x31 | NEEDS_SHIFT, /* O */ 0x18 | NEEDS_SHIFT,
    /* P */ 0x19 | NEEDS_SHIFT, /* Q */ 0x10 | NEEDS_SHIFT, /* R */ 0x13 | NEEDS_SHIFT,
    /* S */ 0x1F | NEEDS_SHIFT, /* T */ 0x14 | NEEDS_SHIFT, /* U */ 0x16 | NEEDS_SHIFT,
    /* V */ 0x2F | NEEDS_SHIFT, /* W */ 0x11 | NEEDS_SHIFT, /* X */ 0x2D | NEEDS_SHIFT,
    /* Y */ 0x15 | NEEDS_SHIFT, /* Z */ 0x2C | NEEDS_SHIFT,
    /* [ */ 0x1A, /* \ */ 0x2B, /* ] */ 0x1B, /* ^ */ 0x07 | NEEDS_SHIFT,
    /* _ */ 0x0C | NEEDS_SHIFT, /* ` */ 0x29,
    /* a */ 0x1E, /* b */ 0x30, /* c */ 0x2E, /* d */ 0x20, /* e */ 0x12,
    /* f */ 0x21, /* g */ 0x22, /* h */ 0x23, /* i */ 0x17, /* j */ 0x24,
    /* k */ 0x25, /* l */ 0x26, /* m */ 0x32, /* n */ 0x31, /* o */ 0x18,
    /* p */ 0x19, /* q */ 0x10, /* r */ 0x13, /* s */ 0x1F, /* t */ 0x14,
    /* u */ 0x16, /* v */ 0x2F, /* w */ 0x11, /* x */ 0x2D, /* y */ 0x15,
    /* z */ 0x2C,
    /* { */ 0x1A | NEEDS_SHIFT, /* | */ 0x2B | NEEDS_SHIFT, /* } */ 0x1B | NEEDS_SHIFT,
    /* ~ */ 0x29 | NEEDS_SHIFT,
};

// ─── Mouse emulation state ─────────────────────────────────────────────────
// Motion is derived from which arrows are currently held, sampled on a timer,
// rather than from key events: the two-phase I2C poll drains one FIFO entry per
// cycle, so event-driven motion would be lumpy and would stall while other keys
// were being reported.
#define MOUSE_STEP_US   16000   // ~60 updates/s
#define MOUSE_SPEED_MIN 3.0f
#define MOUSE_SPEED_MAX 16.0f   // packets carry 6-bit signed deltas (-32..31)
#define MOUSE_ACCEL     0.35f

#define MB_RIGHT 0x01           // sermouseevent(): bit0 right, bit1 left
#define MB_LEFT  0x02

enum { DIR_LEFT, DIR_UP, DIR_DOWN, DIR_RIGHT, DIR_COUNT };

static bool mouse_mode = false;
static bool arrow_held[DIR_COUNT];
static uint8_t mouse_buttons = 0;
static uint8_t mouse_buttons_sent = 0xFF;
static float mouse_speed = MOUSE_SPEED_MIN;
static uint64_t last_mouse_us = 0;
static bool swallow_toggle_key = false;

// Physical Shift, tracked per side so suppression restores exactly what was
// held. See shift_suppress() for why that is needed.
static bool shift_l_down = false, shift_r_down = false;
static bool shift_suppressed = false;
static uint8_t suppress_owner = 0;      // XT code that triggered suppression
#define real_shift_down (shift_l_down || shift_r_down)
static bool synth_shift_down = false;
static bool ctrl_down = false;
static bool alt_down = false;
static uint64_t last_poll_us = 0;
static uint8_t poll_phase = 0;

// ─── I2C helpers ───────────────────────────────────────────────────────────

static bool kbd_write_reg(const uint8_t reg, const uint8_t value) {
    const uint8_t msg[2] = {reg | REG_WRITE_BIT, value};
    return i2c_write_timeout_us(PICOCALC_KBD_I2C, PICOCALC_KBD_ADDR, msg, 2, false, 5000) == 2;
}

// The keyboard MCU needs time between the register-select write and the read.
// The ClockworkPi sample sleeps 16 ms between the two; shapones splits them
// across two 16 ms ISR ticks. Issuing them back-to-back gets stale data or a
// failed read - note that a write-only transaction (the backlight) still works,
// which makes this fault look like "the display is broken" rather than "the
// keyboard is broken". Hence the split: select now, read on the next poll.
static bool kbd_select_reg(const uint8_t reg) {
    return i2c_write_timeout_us(PICOCALC_KBD_I2C, PICOCALC_KBD_ADDR, &reg, 1, false, 5000) == 1;
}

static int kbd_read16(void) {
    uint16_t buf = 0;
    if (i2c_read_timeout_us(PICOCALC_KBD_I2C, PICOCALC_KBD_ADDR, (uint8_t *) &buf, 2, false, 5000) != 2)
        return -1;
    return buf;
}

// Blocking variant for the paths where latency does not matter.
static int kbd_read_reg16(const uint8_t reg) {
    if (!kbd_select_reg(reg)) return -1;
    sleep_ms(16);
    return kbd_read16();
}

void picocalc_lcd_set_backlight(const uint8_t level) {
    kbd_write_reg(REG_ID_BKL, level);
}

void picocalc_kbd_set_backlight(const uint8_t level) {
    kbd_write_reg(REG_ID_BK2, level);
}

int picocalc_read_battery(void) {
    return kbd_read_reg16(REG_ID_BAT);
}

// ─── Scancode emission ─────────────────────────────────────────────────────

// ─── Scancode queue ────────────────────────────────────────────────────────
//
// handleScancode() writes a single byte to port 0x60 - there is no hardware
// queue - and this driver is polled from core0, the same core that runs
// exec86(). The emulated CPU therefore does not execute between two
// consecutive calls, so emitting two scancodes in one poll silently loses the
// first. That is fatal for any key that needs more than one code: suppressing
// Shift before a pre-combined F-key dropped the Shift break, leaving the BIOS
// with Shift latched so F6 arrived as Shift+F6 and was ignored.
//
// So scancodes are queued and released one at a time, with the emulation given
// a slice in between (see picocalc_kbd_pending() and the main loop).
#define SC_QUEUE_SIZE 32
#define SC_GAP_US     1500      // ~1.5 ms between codes; the BIOS ISR needs far less

static uint8_t sc_queue[SC_QUEUE_SIZE];
static uint8_t sc_head = 0, sc_tail = 0;
static uint64_t last_sc_us = 0;

static void sc_push(const uint8_t code) {
    const uint8_t next = (uint8_t) ((sc_head + 1) % SC_QUEUE_SIZE);
    if (next == sc_tail) return;   // full: drop rather than corrupt the order
    sc_queue[sc_head] = code;
    sc_head = next;
}

static inline void send_make(const uint8_t xt) {
    sc_push(xt);
}

static inline void send_break(const uint8_t xt) {
    sc_push(xt | 0x80);
}

bool picocalc_kbd_pending(void) {
    return sc_head != sc_tail;
}

void picocalc_kbd_pump(void) {
    if (sc_head == sc_tail) return;
    const uint64_t now = time_us_64();
    if (now - last_sc_us < SC_GAP_US) return;
    last_sc_us = now;
    handleScancode(sc_queue[sc_tail]);
    sc_tail = (uint8_t) ((sc_tail + 1) % SC_QUEUE_SIZE);
}

// These keys only exist as Shift combinations on this keyboard: the firmware
// folds Shift in and reports a different key code entirely (Shift+F2 -> F7,
// Shift+Tab -> Home, ...). Forwarding the physical Shift as well would make the
// emulated PC see Shift+F7, which is a different BIOS scancode from F7, so the
// application never sees the key that was actually pressed.
static bool is_shift_combined(const uint8_t key) {
    switch (key) {
        case 0x86: case 0x87: case 0x88: case 0x89: // F6-F9
        case KEY_F10:
        case KEY_END: case KEY_HOME: case KEY_INSERT:
        case KEY_BREAK: case KEY_PAGE_UP: case KEY_PAGE_DOWN:
            return true;
        default:
            return false;
    }
}

// Hide the physical Shift from the emulated keyboard for the duration of a
// pre-combined key, then put it back if it is still held.
static void shift_suppress(const uint8_t owner) {
    if (shift_suppressed || !real_shift_down) return;
    if (shift_l_down) send_break(XT_LSHIFT);
    if (shift_r_down) send_break(XT_RSHIFT);
    shift_suppressed = true;
    suppress_owner = owner;
}

static void shift_restore(const uint8_t owner) {
    if (!shift_suppressed || suppress_owner != owner) return;
    if (shift_l_down) send_make(XT_LSHIFT);
    if (shift_r_down) send_make(XT_RSHIFT);
    shift_suppressed = false;
    suppress_owner = 0;
}

// Map a PicoCalc special key to an XT code, 0 if we have no equivalent.
static uint8_t special_to_xt(const uint8_t key) {
    switch (key) {
        case KEY_BACKSPACE: return XT_BACKSPACE;
        case KEY_TAB: return XT_TAB;
        case KEY_ENTER: return XT_ENTER;
        case KEY_ESC: return XT_ESC;
        case KEY_LEFT: return XT_LEFT;
        case KEY_UP: return XT_UP;
        case KEY_DOWN: return XT_DOWN;
        case KEY_RIGHT: return XT_RIGHT;
        case KEY_CAPS_LOCK: return XT_CAPS;
        case KEY_BREAK: return XT_BREAK;
        case KEY_INSERT: return XT_INSERT;
        case KEY_HOME: return XT_HOME;
        case KEY_DEL: return XT_DEL;
        case KEY_END: return XT_END;
        case KEY_PAGE_UP: return XT_PGUP;
        case KEY_PAGE_DOWN: return XT_PGDN;
        default: break;
    }
    // F1-F9 are 0x81-0x89 and F10 is 0x90 (a firmware quirk); XT has them
    // contiguous at 0x3B-0x44.
    if (key >= KEY_F1 && key <= 0x89) return 0x3B + (key - KEY_F1);
    if (key == KEY_F10) return 0x44;
    return 0;
}

// The emulator's own hotkeys live on keypad keys the PicoCalc does not have
// (Ctrl-Alt-KP* toggles EGA/VGA, Ctrl-Alt-KP+/- step the CPU throttle). Since
// those combinations already require Ctrl+Alt, borrow F1/F2/F3 for them - the
// plain function keys are untouched. Ctrl-Alt-Del needs no help: the PicoCalc
// has real Ctrl, Alt and Del keys.
static uint8_t hotkey_substitute(const uint8_t key) {
    if (!(ctrl_down && alt_down)) return 0;
    switch (key) {
        case 0x81: return XT_KP_STAR; // Ctrl-Alt-F1 -> Ctrl-Alt-KP*
        case 0x82: return XT_KP_MINUS; // Ctrl-Alt-F2 -> Ctrl-Alt-KP-
        case 0x83: return XT_KP_PLUS; // Ctrl-Alt-F3 -> Ctrl-Alt-KP+
        default: return 0;
    }
}

static void handle_event(const uint8_t key, const uint8_t state) {
    if (state != KEY_STATE_PRESSED && state != KEY_STATE_HOLD && state != KEY_STATE_RELEASED)
        return;

#ifdef PICOCALC_KBD_DEBUG
    // Log every raw event from the keyboard MCU plus the XT code it maps to, so
    // a key that "does not work" can be told apart from a key that never
    // arrives. This build suppresses the perf counter so the overlay is not
    // scrolled away between pressing a key and reading it.
    printf("K=%02X s=%d xt=%02X %s%s\n", key, state, special_to_xt(key),
           real_shift_down ? "SH" : "--",
           is_shift_combined(key) ? " comb" : "");
#endif

    const bool down = state != KEY_STATE_RELEASED;

    // Ctrl-Alt-M toggles mouse mode. Swallow the matching release too, so the
    // emulated keyboard never sees a break code without its make.
    if (key == 'm' || key == 'M') {
        if (down && ctrl_down && alt_down) {
            if (state == KEY_STATE_PRESSED) {
                mouse_mode = !mouse_mode;
                for (int i = 0; i < DIR_COUNT; i++) arrow_held[i] = false;
                mouse_buttons = 0;
                mouse_speed = MOUSE_SPEED_MIN;
                // The keyboard backlight is otherwise always off, so it is an
                // unambiguous indicator - a normal build renders no overlay.
                picocalc_kbd_set_backlight(mouse_mode ? 0x80 : 0x00);
                printf("MOUSE %s\n", mouse_mode ? "ON" : "OFF");
            }
            swallow_toggle_key = true;
            return;
        }
        if (swallow_toggle_key) {
            if (!down) swallow_toggle_key = false;
            return;
        }
    }

    // In mouse mode the arrows and the two button keys are captured; every
    // other key still reaches DOS as usual.
    if (mouse_mode) {
        int dir = -1;
        switch (key) {
            case KEY_LEFT:  dir = DIR_LEFT;  break;
            case KEY_UP:    dir = DIR_UP;    break;
            case KEY_DOWN:  dir = DIR_DOWN;  break;
            case KEY_RIGHT: dir = DIR_RIGHT; break;
            default: break;
        }
        if (dir >= 0) {
            if (state != KEY_STATE_HOLD) arrow_held[dir] = down;
            return;
        }
        if (key == '[' || key == ']') {
            const uint8_t bit = (key == '[') ? MB_LEFT : MB_RIGHT;
            if (state != KEY_STATE_HOLD) {
                if (down) mouse_buttons |= bit;
                else mouse_buttons &= ~bit;
            }
            return;
        }
    }

    // Real modifiers pass straight through.
    switch (key) {
        case KEY_MOD_SHL:
        case KEY_MOD_SHR: {
            if (state == KEY_STATE_HOLD) return; // already down, don't repeat
            const bool is_left = key == KEY_MOD_SHL;
            if (is_left) shift_l_down = down; else shift_r_down = down;
            // While suppressed the emulated side already believes Shift is up,
            // so do not send a second break for it.
            if (shift_suppressed) return;
            const uint8_t xt = is_left ? XT_LSHIFT : XT_RSHIFT;
            down ? send_make(xt) : send_break(xt);
            return;
        }
        case KEY_MOD_CTRL:
            if (state == KEY_STATE_HOLD) return;
            ctrl_down = down;
            down ? send_make(XT_CTRL) : send_break(XT_CTRL);
            return;
        case KEY_MOD_ALT:
            if (state == KEY_STATE_HOLD) return;
            alt_down = down;
            down ? send_make(XT_ALT) : send_break(XT_ALT);
            return;
        case KEY_MOD_SYM:
            // Dead code on PicoCalc hardware: KEY_MOD_SYM exists in the
            // ClockworkPi firmware headers but the key matrix in
            // picocalc_keyboard/keyboard.ino assigns no physical key to it -
            // Alt, Ctrl and the two Shifts are the only modifiers, and every
            // symbol is a plain or Shift combination. Kept because it costs
            // nothing and other keyboard revisions may differ; if a board does
            // send it, the character is already folded in and there is no PC
            // equivalent, so swallowing it is right either way.
            return;
        default:
            break;
    }

    uint8_t xt = hotkey_substitute(key);
    bool needs_shift = false;
    const bool combined = is_shift_combined(key);

    if (!xt) {
        xt = special_to_xt(key);
        if (!xt) {
            if (key < 0x20 || key > 0x7E) return; // nothing we can express
            const uint8_t entry = ascii_to_xt[key - 0x20];
            xt = entry & 0x7F;
            needs_shift = (entry & NEEDS_SHIFT) != 0;
        }
    }

    if (down) {
        if (combined) shift_suppress(xt);
        // Only synthesise Shift if the user isn't already holding one - if we
        // faked a break for a key they are physically holding, the emulated
        // keyboard would think Shift was up for everything that followed.
        if (needs_shift && !real_shift_down && !synth_shift_down) {
            synth_shift_down = true;
            send_make(XT_LSHIFT);
        }
        send_make(xt);
    } else {
        send_break(xt);
        if (synth_shift_down) {
            synth_shift_down = false;
            send_break(XT_LSHIFT);
        }
        if (combined) shift_restore(xt);
    }
}

bool picocalc_mouse_step(uint8_t *buttons, int8_t *dx, int8_t *dy) {
    if (!mouse_mode) return false;

    const uint64_t now = time_us_64();
    if (now - last_mouse_us < MOUSE_STEP_US) return false;
    last_mouse_us = now;

    const bool moving = arrow_held[DIR_LEFT] || arrow_held[DIR_RIGHT] ||
                        arrow_held[DIR_UP] || arrow_held[DIR_DOWN];

    // Nothing to say: no movement and the buttons already match what the
    // driver last saw. Staying quiet keeps the serial buffer clear.
    if (!moving && mouse_buttons == mouse_buttons_sent) {
        mouse_speed = MOUSE_SPEED_MIN;
        return false;
    }

    if (moving) {
        mouse_speed += MOUSE_ACCEL;
        if (mouse_speed > MOUSE_SPEED_MAX) mouse_speed = MOUSE_SPEED_MAX;
    } else {
        mouse_speed = MOUSE_SPEED_MIN;
    }

    const int8_t step = (int8_t) mouse_speed;
    *dx = arrow_held[DIR_LEFT] ? -step : arrow_held[DIR_RIGHT] ? step : 0;
    *dy = arrow_held[DIR_UP] ? -step : arrow_held[DIR_DOWN] ? step : 0;
    *buttons = mouse_buttons;
    mouse_buttons_sent = mouse_buttons;
    return true;
}

// ─── Public entry points ───────────────────────────────────────────────────

void keyboard_init(void) {
    gpio_set_function(PICOCALC_KBD_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICOCALC_KBD_SCL_PIN, GPIO_FUNC_I2C);
    i2c_init(PICOCALC_KBD_I2C, PICOCALC_KBD_HZ);
    gpio_pull_up(PICOCALC_KBD_SDA_PIN);
    gpio_pull_up(PICOCALC_KBD_SCL_PIN);

    shift_l_down = shift_r_down = shift_suppressed = false;
    suppress_owner = 0;
    synth_shift_down = ctrl_down = alt_down = false;
    poll_phase = 0;
    sc_head = sc_tail = 0;
    mouse_mode = false;
    for (int i = 0; i < DIR_COUNT; i++) arrow_held[i] = false;
    mouse_buttons = 0; mouse_buttons_sent = 0xFF;
    picocalc_kbd_set_backlight(0x00);

    // Drain anything the MCU buffered while we were booting.
    for (int i = 0; i < 16; i++) {
        const int e = kbd_read_reg16(REG_ID_FIF);
        if (e <= 0) break;
    }

    // Backlight on before core1 brings the panel up, so that the panel is
    // visible for the whole of init - including the self-test pattern. The
    // cost is a brief flash of uninitialised panel memory at boot.
    picocalc_lcd_set_backlight(0xFF);
}

// The emulated 8042 forwards writes to port 0x64 to the keyboard (LED state,
// typematic rate, resets). There is no PS/2 keyboard here to forward them to
// and the MCU has no equivalent, so accept and discard them - this stands in
// for the PS/2 driver's keyboard_send().
int16_t keyboard_send(uint8_t data) {
    (void) data;
    return 0;
}

void picocalc_kbd_poll(void) {
    const uint64_t now = time_us_64();
    if (now - last_poll_us < PICOCALC_KBD_POLL_US) return;
    last_poll_us = now;

    // Two-phase, non-blocking: select the FIFO register on one tick, read the
    // event on the next. Never sleeps in the emulation loop.
    if (poll_phase == 0) {
        kbd_select_reg(REG_ID_FIF);
        poll_phase = 1;
        return;
    }
    poll_phase = 0;
    const int e = kbd_read16();
    if (e > 0)
        handle_event((uint8_t) (e >> 8), (uint8_t) (e & 0xFF));
}
