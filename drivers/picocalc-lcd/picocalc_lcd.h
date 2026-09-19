#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "inttypes.h"
#include "stdbool.h"

// ─── PicoCalc panel wiring (ClockworkPi PicoCalc mainboard v2.0) ────────────
// The panel is an ILI9488-compatible ST7365P, 320x320, driven here by PIO as a
// write-only 3-wire-plus-DC SPI target. MISO (GP12) is unused.
//
// The backlight is NOT on a GPIO — it is set over I2C by the keyboard MCU
// (see drivers/picocalc-kbd). There is deliberately no TFT_LED_PIN here.

#ifndef PICOCALC_LCD_CLK_PIN
#define PICOCALC_LCD_CLK_PIN  (10)
#endif
#ifndef PICOCALC_LCD_DATA_PIN
#define PICOCALC_LCD_DATA_PIN (11)
#endif
#ifndef PICOCALC_LCD_CS_PIN
#define PICOCALC_LCD_CS_PIN   (13)
#endif
#ifndef PICOCALC_LCD_DC_PIN
#define PICOCALC_LCD_DC_PIN   (14)
#endif
#ifndef PICOCALC_LCD_RST_PIN
#define PICOCALC_LCD_RST_PIN  (15)
#endif

// Panel clock ceiling. The ILI9488 write cycle is 15 ns min (~66 MHz); 50 MHz
// is what the ClockworkPi reference driver runs at and leaves margin for the
// PicoCalc's trace lengths.
#ifndef PICOCALC_LCD_MAX_HZ
#define PICOCALC_LCD_MAX_HZ (50000000)
#endif

// The ClockworkPi driver runs its whole init sequence at 25 MHz and tiny_agi
// only raises the bus to 50 MHz once lcd_init() has returned. Do the same: the
// controller's internal clock is slow until sleep-out completes, so pushing the
// init sequence at full rate is not something either reference does.
#ifndef PICOCALC_LCD_INIT_HZ
#define PICOCALC_LCD_INIT_HZ (25000000)
#endif

// Paint a colour-bar test pattern at the end of graphics_init(). It exercises
// the whole panel path - PIO, DMA, window, pixel format - without depending on
// the SD card, the emulator or the palette, so it separates "the display driver
// is broken" from "nothing upstream is producing pixels". Set to 0 once the
// board is known good.
#ifndef PICOCALC_LCD_SELFTEST
#define PICOCALC_LCD_SELFTEST (1)
#endif

#define PICOCALC_LCD_WIDTH  (320)
#define PICOCALC_LCD_HEIGHT (320)

// Everything is rendered into a 320-wide line, so 80 columns of the 4x6 font.
// 25 rows is what the emulated text modes use; the panel could show 53.
#define TEXTMODE_COLS (80)
#define TEXTMODE_ROWS (25)

#define RGB888(r, g, b) (((r) << 16) | ((g) << 8) | (b))

void refresh_lcd(void);

// Set by graphics_init() on core1 once the panel is up and blanked.
extern volatile bool picocalc_lcd_ready;

// Paint printf() output over the bottom 80 rows. Settable at runtime so a
// board can be diagnosed without building and flashing a separate firmware.
extern volatile bool picocalc_lcd_overlay;

// Panel bit rate in Hz; must be set before graphics_init() runs on core1.
// Above the ILI9488's nominal 66 MHz the failure mode is visible artefacts,
// not corruption, so this is clamped rather than validated.
extern uint32_t picocalc_lcd_max_hz;

// Permanent stats bar across the top of the panel. It sits in the letterbox
// margin that every mode leaves (40 rows in the worst case), so it costs no
// picture area.
extern volatile bool picocalc_lcd_statusbar;

// Set the bar's text, up to TEXTMODE_COLS characters; repainted only on change.
void picocalc_lcd_set_status(const char *text);

// Frames pushed to the panel since boot.
extern volatile uint32_t picocalc_lcd_frames;

// The panel SPI rate actually in effect, read back from the PIO clock divider
// rather than recomputed from the build-time define, so an option that failed
// to reach the compiler is visible on the device instead of silently ignored.
uint32_t picocalc_lcd_achieved_mhz(void);

// Allow the driver to pump audio/timer work while it waits on the panel DMA.
// Must not be called until the emulator's sound state exists.
void picocalc_lcd_enable_yield(void);

// Set the panel + keyboard backlight (0..255). Implemented by the keyboard
// driver, since on this board the keyboard MCU owns the backlight.
void picocalc_lcd_set_backlight(uint8_t level);

static inline void graphics_set_bgcolor(uint32_t color888) {
    // dummy - the panel is fully repainted every frame
}

static inline void graphics_set_flashmode(bool flash_line, bool flash_frame) {
    // dummy
}

#ifdef __cplusplus
}
#endif
