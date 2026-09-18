#pragma GCC optimize("O3")
//
// PicoCalc display driver — ILI9488/ST7365P 320x320 over PIO SPI.
//
// The mode unpackers here are ports of the ones in drivers/vga-nextgen/vga.c
// and drivers/hdmi/hdmi.c, which are the two drivers that track the planar
// VIDEORAM layout introduced by commit 0e23cc8 ("256kb vga"). VIDEORAM is a
// uint32_t[65536] indexed by the offset into the 0xA0000 window, each element
// holding four plane bytes [P3|P2|P1|P0]; non-planar modes live in the low
// byte. (drivers/st7789 predates that change and still does byte-packed
// indexing, so it is NOT a valid reference.)
//
// The panel runs in 16-bit mode (0x3A = 0x65): RGB565, two bytes per pixel,
// low byte first (see panel565()). The controller also offers 18-bit (0x66, three bytes), but
// 16-bit is a third less traffic for colour depth that is invisible here - the
// emulated modes use at most a 256-entry palette on a 320x320 panel.
//
// Rendering is two-stage per scanline: unpack the mode into a line of palette
// indices, then expand indices to packed RGB words and DMA them out. 640-wide
// modes are decimated 2:1 horizontally into the same 320-pixel line.

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "emulator/emulator.h"
#include "graphics.h"

#include "picocalc_lcd.pio.h"

extern int cursor_blink_state;

#define PIO_LCD pio0

static uint sm_lcd = 0;
static uint lcd_dma_chan = 0;
static uint pio_offset = 0;

// Palette entries are RGB565, ready to shift straight out to the panel.
static uint16_t palette[256];

// 0x00RRGGBB -> RGB565.
#define RGB565(c) ((uint16_t) ((((c) >> 8) & 0xF800) | (((c) >> 5) & 0x07E0) | (((c) >> 3) & 0x001F)))

// This panel latches each 16-bit pixel LOW BYTE FIRST, unlike the 18-bit mode
// where the first byte sent is the red channel. Determined on hardware: with
// the bytes the other way round, CGA brown (197,125,0) renders as (230,24,24) -
// bright red with the green gone - and light grey (197,198,197) as (57,24,49),
// i.e. everything dark. White survives either way because 0xFFFF is a
// palindrome, which is why the display stayed readable while every other colour
// was wrong.
//
// Swapping here rather than in the packing loop keeps that loop a straight
// two-pixels-per-word copy and costs nothing per frame.
#ifndef PICOCALC_LCD_PIXEL_SWAP
#define PICOCALC_LCD_PIXEL_SWAP 1
#endif

static inline uint16_t panel565(const uint32_t color888) {
    const uint16_t v = RGB565(color888);
#if PICOCALC_LCD_PIXEL_SWAP
    return (uint16_t) ((v >> 8) | (v << 8));
#else
    return v;
#endif
}

uint8_t *text_buffer = NULL;
static uint8_t *graphics_framebuffer = NULL;
static uint framebuffer_width = 0;
static uint framebuffer_height = 0;
static int framebuffer_offset_x = 0;
static int framebuffer_offset_y = 0;

enum graphics_mode_t graphics_mode = TEXTMODE_80x25_COLOR;

// Set once the panel is initialised and blanked. core0 waits on this before
// switching the backlight on, so the user never sees an uninitialised panel -
// and so that only core0 ever drives the keyboard MCU's I2C bus.
volatile bool picocalc_lcd_ready = false;

// Frames pushed to the panel; read by the perf counter in pico-main.c.
volatile uint32_t picocalc_lcd_frames = 0;

// One scanline of palette indices. 640 wide so the hi-res modes can unpack at
// native width before being decimated on the way into the word buffer.
static uint8_t __aligned(4) idx_line[640];
// Two packed-pixel line buffers: one is being filled while the other DMAs.
// 320 px * 2 bytes = 640 bytes = 160 words exactly, so lines never straddle.
#define LINE_WORDS (PICOCALC_LCD_WIDTH * 2 / 4)
static uint32_t __aligned(4) word_line[2][LINE_WORDS];

// Called while blocked on the panel DMA. A full frame takes ~31 ms at 50 MHz,
// which is far longer than the 22.6 us audio sample period, so core1's sound
// pump has to run from inside this driver or audio breaks up. pico-main.c
// overrides this; the weak default keeps the driver standalone-linkable.
__attribute__((weak)) void lcd_yield(void) {
}

// The yield runs the emulator's audio mixer, which dereferences state that does
// not exist yet while graphics_init() is still running - second_core() creates
// the OPL instance only after graphics_init() returns. So the yield stays off
// until the owner says the rest of core1 is built.
static volatile bool yield_enabled = false;

void picocalc_lcd_enable_yield(void) {
    yield_enabled = true;
}

uint32_t picocalc_lcd_achieved_mhz(void) {
    const uint32_t reg = PIO_LCD->sm[sm_lcd].clkdiv;
    const float div = (float) (reg >> 16) + (float) ((reg >> 8) & 0xFF) / 256.0f;
    if (div <= 0.0f) return 0;
    return (uint32_t) ((float) clock_get_hz(clk_sys) / (2.0f * div) / 1000000.0f);
}

// ─── Panel plumbing ────────────────────────────────────────────────────────

static inline void lcd_set_dc_cs(const bool dc, const bool cs) {
    sleep_us(1);
    gpio_put_masked((1u << PICOCALC_LCD_DC_PIN) | (1u << PICOCALC_LCD_CS_PIN),
                    (!!dc << PICOCALC_LCD_DC_PIN) | (!!cs << PICOCALC_LCD_CS_PIN));
    sleep_us(1);
}

// cmd[0] is the command byte, the rest are its parameters.
static void lcd_write_cmd(const uint8_t *cmd, size_t count) {
    pcalc_lcd_wait_idle(PIO_LCD, sm_lcd);
    pcalc_lcd_set_pull_threshold(PIO_LCD, sm_lcd, 8);
    lcd_set_dc_cs(0, 0);
    pcalc_lcd_put8(PIO_LCD, sm_lcd, *cmd++);
    if (count >= 2) {
        pcalc_lcd_wait_idle(PIO_LCD, sm_lcd);
        lcd_set_dc_cs(1, 0);
        for (size_t i = 0; i < count - 1; ++i)
            pcalc_lcd_put8(PIO_LCD, sm_lcd, *cmd++);
    }
    pcalc_lcd_wait_idle(PIO_LCD, sm_lcd);
    lcd_set_dc_cs(1, 1);
}

// ILI9488 power-on sequence, ported from the ClockworkPi reference driver
// (PicoCalc/Code/picocalc_helloworld/lcdspi/lcdspi.c, pico_lcd_init()).
// Format: length including the command byte, post-delay in ms, then the bytes.
static const uint8_t init_seq[] = {
    16, 0, 0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
    16, 0, 0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
    3, 0, 0xC0, 0x17, 0x15, // Power Control 1
    2, 0, 0xC1, 0x41, // Power Control 2
    4, 0, 0xC5, 0x00, 0x12, 0x80, // VCOM Control
    2, 0, 0x36, 0x48, // MADCTL: MX | BGR
    2, 0, 0x3A, 0x65, // Pixel format: 16 bit RGB565, 2 bytes/pixel
    2, 0, 0xB0, 0x00, // Interface Mode Control
    2, 0, 0xB1, 0xA0, // Frame Rate Control
    1, 0, 0x21, // Inversion on
    2, 0, 0xB4, 0x02, // Display Inversion Control
    4, 0, 0xB6, 0x02, 0x02, 0x3B, // Display Function Control
    2, 0, 0xB7, 0xC6, // Entry Mode Set
    2, 0, 0xE9, 0x00,
    5, 0, 0xF7, 0xA9, 0x51, 0x2C, 0x82, // Adjust Control 3
    1, 120, 0x11, // Sleep out
    1, 120, 0x29, // Display on
    0 // terminator
};

// The first E0/E1 entries are longer than one byte of length can describe if
// written naively; they are 15 and 16 bytes including the command, which fits.
static void lcd_run_init_seq(const uint8_t *seq) {
    while (*seq) {
        lcd_write_cmd(seq + 2, *seq);
        sleep_ms(seq[1]);
        seq += *seq + 2;
    }
}

static void lcd_set_window(const uint16_t x, const uint16_t y,
                           const uint16_t width, const uint16_t height) {
    const uint16_t x1 = x + width - 1;
    const uint16_t y1 = y + height - 1;
    uint8_t caset[5] = {0x2A, x >> 8, x & 0xFF, x1 >> 8, x1 & 0xFF};
    uint8_t raset[5] = {0x2B, y >> 8, y & 0xFF, y1 >> 8, y1 & 0xFF};
    lcd_write_cmd(caset, 5);
    lcd_write_cmd(raset, 5);
}

// Issue RAMWR and leave the bus in pixel-streaming state (CS low, DC high,
// autopull threshold 32 so whole words shift out MSB-first).
static inline void start_pixels(void) {
    const uint8_t cmd = 0x2C;
    lcd_write_cmd(&cmd, 1);
    pcalc_lcd_wait_idle(PIO_LCD, sm_lcd);
    lcd_set_dc_cs(1, 0);
    pcalc_lcd_set_pull_threshold(PIO_LCD, sm_lcd, 32);
}

static inline void stop_pixels(void) {
    pcalc_lcd_wait_idle(PIO_LCD, sm_lcd);
    lcd_set_dc_cs(1, 1);
    pcalc_lcd_set_pull_threshold(PIO_LCD, sm_lcd, 8);
}

static inline void send_words(const uint32_t *words, const uint num_words) {
    while (dma_channel_is_busy(lcd_dma_chan)) {
        if (yield_enabled) lcd_yield();
    }
    dma_channel_set_read_addr(lcd_dma_chan, words, false);
    dma_channel_set_trans_count(lcd_dma_chan, num_words, true);
}

// ─── Index line -> packed RGB565 words ─────────────────────────────────────
//
// Two pixels (4 bytes) per word. The PIO shifts MSB-first from bit 31, and the
// panel wants each pixel high byte first, so the left pixel occupies 31:16.
static inline void pack2(uint32_t *out, const uint16_t c0, const uint16_t c1) {
    *out = ((uint32_t) c0 << 16) | (uint32_t) c1;
}

static void pack_line_320(const uint8_t *idx, uint32_t *out) {
    for (int i = 0; i < PICOCALC_LCD_WIDTH / 2; i++) {
        pack2(out++, palette[idx[0]], palette[idx[1]]);
        idx += 2;
    }
}

// 2:1 horizontal decimation. Preferring the left pixel when it is non-zero
// keeps thin bright strokes alive - dropping every odd column outright makes
// 640-wide mono text and hairlines disappear.
static void pack_line_640(const uint8_t *idx, uint32_t *out) {
    for (int i = 0; i < PICOCALC_LCD_WIDTH / 2; i++) {
        const uint8_t a0 = idx[0] ? idx[0] : idx[1];
        const uint8_t a1 = idx[2] ? idx[2] : idx[3];
        pack2(out++, palette[a0], palette[a1]);
        idx += 4;
    }
}

// ─── Mode geometry ─────────────────────────────────────────────────────────

typedef struct {
    uint16_t src_w; // 320 or 640 - selects the packer
    uint16_t out_h; // rows actually pushed to the panel
    uint16_t y_off; // top margin, to centre in 320 rows
    uint8_t vstep; // 1, or 2 to drop every other source line
} lcd_geometry_t;

static lcd_geometry_t mode_geometry(const enum graphics_mode_t mode) {
    switch (mode) {
        case TEXTMODE_40x25_BW:
        case TEXTMODE_40x25_COLOR:
            // 40 columns of the 8x8 font fills the panel width exactly.
            return (lcd_geometry_t){320, 200, 60, 1};

        case TEXTMODE_80x25_BW:
        case TEXTMODE_80x25_COLOR:
            // 80 columns only fit at 4 px each, so 25 rows of the 4x6 font.
            // This is the whole point of the 320x320 panel: real 80-column DOS
            // text with no horizontal decimation.
            return (lcd_geometry_t){320, 150, 85, 1};

        case CGA_640x200x2:
        case TGA_640x200x16:
        case EGA_640x200x16x4:
            return (lcd_geometry_t){640, 200, 60, 1};

        case EGA_640x350x16x4:
            return (lcd_geometry_t){640, 175, 72, 2};

        case VGA_640x480x16:
        case VGA_640x480x2:
        case HERC_640x480x2:
            return (lcd_geometry_t){640, 240, 40, 2};

        case HERC_640x480x2_90:
            return (lcd_geometry_t){640, 174, 73, 2};

        default:
            // Everything else is a 320x200 mode.
            return (lcd_geometry_t){320, 200, 60, 1};
    }
}

// ─── Scanline unpackers ────────────────────────────────────────────────────

// Spread 8 bits of a byte into positions 0,4,8,...28 (copied from
// drivers/vga-nextgen/vga.c, which is where the planar unpackers live).
static inline uint32_t spread8(uint32_t plane) {
    plane = (plane | (plane << 12)) & 0x000F000Fu;
    plane = (plane | (plane << 6)) & 0x03030303u;
    plane = (plane | (plane << 3)) & 0x11111111u;
    return plane;
}

// Merge 4 plane bytes [P3|P2|P1|P0] into 8 nibbles (pixel colour indices).
static inline uint32_t ega_pack8_from_planes(const uint32_t ega_planes) {
    const uint32_t pixel1 = spread8(ega_planes & 0xFFu);
    const uint32_t pixel2 = spread8((ega_planes >> 8) & 0xFFu);
    const uint32_t pixel3 = spread8((ega_planes >> 16) & 0xFFu);
    const uint32_t pixel4 = spread8(ega_planes >> 24);

    return pixel1 | pixel2 << 1 | pixel3 << 2 | pixel4 << 3;
}

// 40x25 with the 8x8 font: 40 columns * 8 px = the full 320, 25 rows * 8 = 200.
static void render_text40_line(const uint y, uint8_t *out) {
    const uint8_t y_div_8 = y >> 3;
    const uint8_t glyph_line = y & 7;
    const uint32_t *text_buffer_line = &VIDEORAM[0x8000 + (vram_offset << 1) + __fast_mul(y_div_8, 80)];

    for (uint column = 0; column < 40; column++) {
        uint8_t glyph_pixels = font_8x8[__fast_mul(*text_buffer_line++ & 0xFF, 8) + glyph_line];
        const uint8_t color = *text_buffer_line++;

        const uint8_t cursor_active = cursor_blink_state &&
                                      y_div_8 == CURSOR_Y && column == CURSOR_X &&
                                      (cursor_start > cursor_end
                                           ? !(glyph_line >= cursor_end && glyph_line <= cursor_start)
                                           : glyph_line >= cursor_start && glyph_line <= cursor_end);

        if (cursor_active) {
            const uint8_t fg = color & 0xF;
            for (int bit = 8; bit--;) *out++ = fg;
        } else {
            if (cga_blinking && (color >> 7 & 1) && cursor_blink_state)
                glyph_pixels = 0;
#pragma GCC unroll(8)
            for (int bit = 8; bit--;) {
                *out++ = glyph_pixels & 1 ? color & 0xF : (color >> 4) & 0x7;
                glyph_pixels >>= 1;
            }
        }
    }
}

#if PICOCALC_LCD_SELFTEST
// printf() on this platform does not reach a serial port: pico-main.c's
// _putchar() writes into DEBUG_VRAM instead. Painting it is a BRING-UP AID
// ONLY - with PICOCALC_BRINGUP=OFF the panel shows the emulator's screen and
// nothing else.
extern uint8_t __aligned(4) DEBUG_VRAM[80 * 10];

// Bring-up variant: the same buffer in the 8x8 font, 40 columns wide. The boot
// messages are all shorter than 40 characters, and at 4x6 they are very hard to
// tell apart from noise on this panel. 10 rows * 8 px = 80, painted over the
// bottom of the screen so it is legible whatever mode the emulator is in.
#define DEBUG_OVERLAY8_ROWS 80

static void render_debug_line8(const uint y, uint8_t *out) {
    static const uint8_t colors[4] = {0x1f, 0xf0, 0x1a, 0x1c};
    const uint8_t y_div_8 = y >> 3;
    const uint8_t glyph_line = y & 7;
    const uint8_t *line = &DEBUG_VRAM[__fast_mul(y_div_8, 80)];

    for (uint column = 0; column < 40; column++) {
        const uint8_t character = *line++;
        const uint8_t attr = colors[character >> 6];
        uint8_t glyph_pixels = font_8x8[__fast_mul(32 + (character & 63), 8) + glyph_line];
#pragma GCC unroll(8)
        for (int bit = 8; bit--;) {
            *out++ = glyph_pixels & 1 ? attr & 0xF : (attr >> 4) & 0xF;
            glyph_pixels >>= 1;
        }
    }
}
#endif

static void render_text_line(const uint y, uint8_t *out) {
    const uint8_t y_div_6 = y / 6;
    const uint8_t glyph_line = y - y_div_6 * 6;
    const uint32_t *text_buffer_line = &VIDEORAM[0x8000 + (vram_offset << 1) + __fast_mul(y_div_6, 160)];

    for (uint column = 0; column < TEXTMODE_COLS; column++) {
        uint8_t glyph_pixels = font_4x6[__fast_mul(*text_buffer_line++ & 0xFF, 6) + glyph_line];
        const uint8_t color = *text_buffer_line++;

        const uint8_t cursor_active = cursor_blink_state &&
                                      y_div_6 == CURSOR_Y && column == CURSOR_X &&
                                      glyph_line >= 4;

        if (cursor_active) {
            const uint8_t fg = color & 0xF;
            *out++ = fg;
            *out++ = fg;
            *out++ = fg;
            *out++ = fg;
        } else {
            if (cga_blinking && (color >> 7 & 1) && cursor_blink_state)
                glyph_pixels = 0;
#pragma GCC unroll(4)
            for (int bit = 4; bit--;) {
                *out++ = glyph_pixels & 1 ? color & 0xF : (color >> 4) & 0x7;
                glyph_pixels >>= 1;
            }
        }
    }
}

static void render_graphics_line(const enum graphics_mode_t mode, const uint y, uint8_t *out) {
    switch (mode) {
        case CGA_320x200x4:
        case CGA_320x200x4_BW: {
            const uint32_t *cga_row = &VIDEORAM[0x8000 + (vram_offset << 1) + __fast_mul(y >> 1, 80) + ((y & 1) << 13)];
            for (int x = 320 / 4; x--;) {
                const uint8_t cga_byte = *cga_row++;
                *out++ = cga_byte >> 6 & 3;
                *out++ = cga_byte >> 4 & 3;
                *out++ = cga_byte >> 2 & 3;
                *out++ = cga_byte & 3;
            }
            break;
        }

        case CGA_640x200x2: {
            const uint32_t *cga_row = &VIDEORAM[0x8000 + (vram_offset << 1) + __fast_mul(y >> 1, 80) + ((y & 1) << 13)];
            for (int x = 640 / 8; x--;) {
                const uint8_t cga_byte = *cga_row++ & 0xFF;
#pragma GCC unroll(8)
                for (int bit = 8; bit--;)
                    *out++ = (cga_byte >> bit) & 1 ? cga_foreground_color : 0;
            }
            break;
        }

        case COMPOSITE_160x200x16_force:
        case COMPOSITE_160x200x16:
        case TGA_160x200x16: {
            const uint32_t *tga_row = &VIDEORAM[tga_offset + __fast_mul(y >> 1, 80) + ((y & 1) << 13)];
            for (int x = 320 / 4; x--;) {
                const uint8_t two_pixels = *tga_row++;
                const uint8_t c1 = two_pixels >> 4;
                const uint8_t c2 = two_pixels & 15;
                *out++ = c1;
                *out++ = c1;
                *out++ = c2;
                *out++ = c2;
            }
            break;
        }

        case TGA_320x200x16: {
            const uint32_t *tga_row = &VIDEORAM[tga_offset + (y & 3) * 8192 + __fast_mul(y >> 2, 160)];
            for (int x = 320 / 2; x--;) {
                const uint8_t two_pixels = *tga_row++;
                *out++ = two_pixels >> 4;
                *out++ = two_pixels & 15;
            }
            break;
        }

        case TGA_640x200x16: {
            const uint32_t *tga_row = &VIDEORAM[__fast_mul(y, 320)];
            for (int x = 640 / 2; x--;) {
                const uint8_t two_pixels = *tga_row++;
                *out++ = two_pixels >> 4;
                *out++ = two_pixels & 15;
            }
            break;
        }

        case EGA_320x200x16x4: {
            const uint32_t *ega_row = &VIDEORAM[__fast_mul(y, 40)];
            for (int x = 0; x < 40; x++) {
                const uint32_t eight_pixels = ega_pack8_from_planes(*ega_row++);
                *out++ = eight_pixels >> 28;
                *out++ = eight_pixels >> 24 & 0xF;
                *out++ = eight_pixels >> 20 & 0xF;
                *out++ = eight_pixels >> 16 & 0xF;
                *out++ = eight_pixels >> 12 & 0xF;
                *out++ = eight_pixels >> 8 & 0xF;
                *out++ = eight_pixels >> 4 & 0xF;
                *out++ = eight_pixels & 0xF;
            }
            break;
        }

        case EGA_640x200x16x4:
        case EGA_640x350x16x4:
        case VGA_640x480x16: {
            const uint32_t *ega_row = &VIDEORAM[__fast_mul(y, 80)];
            for (int i = 0; i < 80; i++) {
                const uint32_t eight_pixels = ega_pack8_from_planes(*ega_row++);
                *out++ = eight_pixels >> 28;
                *out++ = eight_pixels >> 24 & 0xF;
                *out++ = eight_pixels >> 20 & 0xF;
                *out++ = eight_pixels >> 16 & 0xF;
                *out++ = eight_pixels >> 12 & 0xF;
                *out++ = eight_pixels >> 8 & 0xF;
                *out++ = eight_pixels >> 4 & 0xF;
                *out++ = eight_pixels & 0xF;
            }
            break;
        }

        case VGA_640x480x2: {
            const uint32_t *vga_row = &VIDEORAM[__fast_mul(y, 80)];
            for (int x = 640 / 8; x--;) {
                const uint8_t byte = *vga_row++;
#pragma GCC unroll(8)
                for (int bit = 8; bit--;)
                    *out++ = (byte >> bit) & 1 ? 15 : 0;
            }
            break;
        }

        case HERC_640x480x2_90:
        case HERC_640x480x2: {
            const uint32_t *herc_row = mode == HERC_640x480x2_90
                                           ? &VIDEORAM[5 + (y & 3) * 8192 + __fast_mul(y >> 2, 90)]
                                           : &VIDEORAM[(y & 3) * 8192 + __fast_mul(y >> 2, 90)];
            for (int x = 640 / 8; x--;) {
                const uint8_t byte = *herc_row++;
#pragma GCC unroll(8)
                for (int bit = 8; bit--;)
                    *out++ = (byte >> bit) & 1 ? 15 : 0;
            }
            break;
        }

        case VGA_320x200x256x4: {
            // Chained/planar 256-colour: one dword holds four consecutive pixels.
            const uint32_t *vga_row = &VIDEORAM[__fast_mul(y, 80)];
            uint32_t *out32 = (uint32_t *) out;
            for (int x = 0; x < 80; x++)
                *out32++ = *vga_row++;
            break;
        }

        case VGA_320x200x256:
        default: {
            const uint32_t *vga_row = &VIDEORAM[__fast_mul(y, 320)];
            for (int x = 320; x--;)
                *out++ = *vga_row++ & 0xFF;
            break;
        }
    }
}

// ─── Frame push ────────────────────────────────────────────────────────────

void __time_critical_func(refresh_lcd)(void) {
    picocalc_lcd_frames++;
    const enum graphics_mode_t mode = graphics_mode;
    const lcd_geometry_t g = mode_geometry(mode);
    const bool is_text40 = mode == TEXTMODE_40x25_BW || mode == TEXTMODE_40x25_COLOR;
    const bool is_text80 = mode == TEXTMODE_80x25_BW || mode == TEXTMODE_80x25_COLOR;

    lcd_set_window(0, g.y_off, PICOCALC_LCD_WIDTH, g.out_h);
    start_pixels();

    int cur = 0;
    for (uint row = 0; row < g.out_h; row++) {
        const uint src_y = g.vstep == 2 ? row * 2 : row;

        if (is_text80)
            render_text_line(src_y, idx_line);
        else if (is_text40)
            render_text40_line(src_y, idx_line);
        else
            render_graphics_line(mode, src_y, idx_line);

        if (g.src_w == 640)
            pack_line_640(idx_line, word_line[cur]);
        else
            pack_line_320(idx_line, word_line[cur]);

        send_words(word_line[cur], LINE_WORDS);
        cur ^= 1;
    }

    while (dma_channel_is_busy(lcd_dma_chan)) {
        if (yield_enabled) lcd_yield();
    }
    stop_pixels();

#if PICOCALC_LCD_SELFTEST
    // Bring-up: legible diagnostics over the bottom of the panel, whatever mode
    // is active. Turn PICOCALC_LCD_SELFTEST off to get the picture back.
    {
        const uint overlay_y = PICOCALC_LCD_HEIGHT - DEBUG_OVERLAY8_ROWS;
        // Blue background so it is unmistakably a text panel and not noise.
        palette[1] = panel565(0x000080u);
        palette[0x0a] = panel565(0x40FF40u);
        palette[0x0c] = panel565(0xFF6060u);
        palette[0x0f] = panel565(0xFFFFFFu);
        lcd_set_window(0, overlay_y, PICOCALC_LCD_WIDTH, DEBUG_OVERLAY8_ROWS);
        start_pixels();
        for (uint row = 0; row < DEBUG_OVERLAY8_ROWS; row++) {
            render_debug_line8(row, idx_line);
            pack_line_320(idx_line, word_line[cur]);
            send_words(word_line[cur], LINE_WORDS);
            cur ^= 1;
        }
        while (dma_channel_is_busy(lcd_dma_chan)) {
            if (yield_enabled) lcd_yield();
        }
        stop_pixels();
    }
    return;
#endif

}

// ─── graphics.h API ────────────────────────────────────────────────────────

void graphics_set_mode(const enum graphics_mode_t mode) {
    graphics_mode = mode;
    // Repaint the letterbox margins: a mode change can shrink the active area
    // and leave the previous mode's pixels stranded top and bottom.
    const lcd_geometry_t g = mode_geometry(mode);
    memset(word_line[0], 0, sizeof word_line[0]);
    if (g.y_off) {
        lcd_set_window(0, 0, PICOCALC_LCD_WIDTH, g.y_off);
        start_pixels();
        for (uint row = 0; row < g.y_off; row++)
            send_words(word_line[0], LINE_WORDS);
        while (dma_channel_is_busy(lcd_dma_chan)) {
        }
        stop_pixels();
    }
    const uint bottom = PICOCALC_LCD_HEIGHT - g.y_off - g.out_h;
    if (bottom) {
        lcd_set_window(0, g.y_off + g.out_h, PICOCALC_LCD_WIDTH, bottom);
        start_pixels();
        for (uint row = 0; row < bottom; row++)
            send_words(word_line[0], LINE_WORDS);
        while (dma_channel_is_busy(lcd_dma_chan)) {
        }
        stop_pixels();
    }
}

void graphics_set_palette(const uint8_t index, const uint32_t color888) {
    palette[index] = panel565(color888);
}

void graphics_set_buffer(uint8_t *buffer, const uint16_t width, const uint16_t height) {
    graphics_framebuffer = buffer;
    framebuffer_width = width;
    framebuffer_height = height;
}

void graphics_set_textbuffer(uint8_t *buffer) {
    text_buffer = buffer;
}

void graphics_set_offset(const int x, const int y) {
    framebuffer_offset_x = x;
    framebuffer_offset_y = y;
}

void clrScr(const uint8_t color) {
    uint16_t *t_buf = (uint16_t *) text_buffer;
    int size = TEXTMODE_COLS * TEXTMODE_ROWS;
    while (size--) *t_buf++ = color << 4 | ' ';
}

void graphics_init(void) {
    gpio_init(PICOCALC_LCD_CS_PIN);
    gpio_init(PICOCALC_LCD_DC_PIN);
    gpio_init(PICOCALC_LCD_RST_PIN);
    gpio_set_dir(PICOCALC_LCD_CS_PIN, GPIO_OUT);
    gpio_set_dir(PICOCALC_LCD_DC_PIN, GPIO_OUT);
    gpio_set_dir(PICOCALC_LCD_RST_PIN, GPIO_OUT);
    gpio_put(PICOCALC_LCD_CS_PIN, 1);

    // Two PIO cycles per bit, so divide down to the target bit rate.
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    float init_div = (float) sys_hz / (2.0f * (float) PICOCALC_LCD_INIT_HZ);
    float fast_div = (float) sys_hz / (2.0f * (float) PICOCALC_LCD_MAX_HZ);
    if (init_div < 1.0f) init_div = 1.0f;
    if (fast_div < 1.0f) fast_div = 1.0f;

    pio_offset = pio_add_program(PIO_LCD, &pcalc_lcd_program);
    sm_lcd = pio_claim_unused_sm(PIO_LCD, true);
    // Bring the panel up at the slower rate the reference drivers use.
    pcalc_lcd_program_init(PIO_LCD, sm_lcd, pio_offset,
                           PICOCALC_LCD_DATA_PIN, PICOCALC_LCD_CLK_PIN, init_div);

    // Hardware reset: the ILI9488 needs a long settle after RST goes high.
    gpio_put(PICOCALC_LCD_RST_PIN, 1);
    sleep_ms(10);
    gpio_put(PICOCALC_LCD_RST_PIN, 0);
    sleep_ms(10);
    gpio_put(PICOCALC_LCD_RST_PIN, 1);
    sleep_ms(200);

    lcd_run_init_seq(init_seq);

    // Init done - the controller will take the full rate from here on.
    pcalc_lcd_wait_idle(PIO_LCD, sm_lcd);
    pio_sm_set_clkdiv(PIO_LCD, sm_lcd, fast_div);
    pio_sm_clkdiv_restart(PIO_LCD, sm_lcd);

    for (int i = 0; i < 256; i++) palette[i] = 0;

    lcd_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(lcd_dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_dreq(&c, pio_get_dreq(PIO_LCD, sm_lcd, true));
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(lcd_dma_chan, &c, &PIO_LCD->txf[sm_lcd], NULL, 0, false);

#if PICOCALC_LCD_SELFTEST
    // Colour bars over the whole panel, straight from literal RGB values. If
    // this does not appear, the fault is in this driver or the wiring; if it
    // does, the panel path works and the problem is upstream.
    {
        static const uint32_t bars[8] = {
            0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
            0xFF00FF, 0xFF0000, 0x0000FF, 0x808080,
        };
        for (int i = 0; i < 320; i++)
            idx_line[i] = i * 8 / 320; // 8 bars of 40 px
        // Borrow the palette for the pattern, then clear it again below.
        for (int i = 0; i < 8; i++) palette[i] = panel565(bars[i]);

        lcd_set_window(0, 0, PICOCALC_LCD_WIDTH, PICOCALC_LCD_HEIGHT);
        start_pixels();
        pack_line_320(idx_line, word_line[0]);
        for (int row = 0; row < PICOCALC_LCD_HEIGHT; row++)
            send_words(word_line[0], LINE_WORDS);
        while (dma_channel_is_busy(lcd_dma_chan)) {
        }
        stop_pixels();
        sleep_ms(2000);
    }
#endif

    // Blank the whole panel once so the margins start black.
    memset(word_line[0], 0, sizeof word_line[0]);
    for (int i = 0; i < 256; i++) palette[i] = 0;
    lcd_set_window(0, 0, PICOCALC_LCD_WIDTH, PICOCALC_LCD_HEIGHT);
    start_pixels();
    for (int row = 0; row < PICOCALC_LCD_HEIGHT; row++)
        send_words(word_line[0], LINE_WORDS);
    while (dma_channel_is_busy(lcd_dma_chan)) {
    }
    stop_pixels();

    picocalc_lcd_ready = true;
}
