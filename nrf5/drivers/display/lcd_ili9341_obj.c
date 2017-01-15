/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Glenn Ruben Bakke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "genhdr/pins.h"

#include "lcd_ili9341_driver.h"
// For now PWM is only enabled for nrf52 targets.
#if MICROPY_PY_DISPLAY_LCD_ILI9341

/// \moduleref display
/// \class ILI9341 - ILI9341 TFT LCD display driver.

#include "pin.h"
#include "spi.h"
#include "lcd_mono_fb.h"

typedef struct _lcd_ili9341_obj_t {
    mp_obj_base_t base;
    machine_hard_spi_obj_t *spi;
    pin_obj_t * pin_cs;
    pin_obj_t * pin_dc;
    mp_obj_framebuf_t * framebuffer;
} lcd_ili9341_obj_t;

static void dirty_line_update_cb(mp_obj_framebuf_t * p_framebuffer,
                                 uint16_t            line,
                                 fb_byte_t *         p_new,
                                 fb_byte_t *         p_old) {
    // the lcd does not have double buffer needs, skip it.
    (void)p_old;

    driver_ili9341_update_line(line, p_new, p_framebuffer->bytes_stride, true);
}

/// \method __str__()
/// Return a string describing the ILI9341 object.
STATIC void lcd_ili9341_print(const mp_print_t *print, mp_obj_t o, mp_print_kind_t kind) {
    lcd_ili9341_obj_t *self = o;

    mp_printf(print, "ILI9341(SPI(mosi=(port=%u, pin=%u), miso=(port=%u, pin=%u), clk=(port=%u, pin=%u)),\n",
                     self->spi->pyb->spi->init.mosi_pin_port,
                     self->spi->pyb->spi->init.mosi_pin,
                     self->spi->pyb->spi->init.miso_pin_port,
                     self->spi->pyb->spi->init.miso_pin,
                     self->spi->pyb->spi->init.clk_pin_port,
                     self->spi->pyb->spi->init.clk_pin
                     );

    mp_printf(print, "        cs=(port=%u, pin=%u), dc=(port=%u, pin=%u),\n",
                     self->pin_cs->port,
                     self->pin_cs->pin,
                     self->pin_dc->port,
                     self->pin_dc->pin);

    mp_printf(print, "        FB(width=%u, height=%u, dir=%u))\n",
                     self->framebuffer->width,
                     self->framebuffer->height);
}

// for make_new
enum {
    ARG_NEW_WIDTH,
    ARG_NEW_HEIGHT,
    ARG_NEW_SPI,
    ARG_NEW_CS,
    ARG_NEW_DC,
};

/*
Example for nrf52832 / pca10040:

from machine import Pin, SPI
from display import ILI9341
cs = Pin("A16", mode=Pin.OUT, pull=Pin.PULL_UP)
dc = Pin("A17", mode=Pin.OUT, pull=Pin.PULL_UP)
spi = SPI(0, baudrate=8000000)
d = ILI9341(240, 320, spi, cs, dc)
d.text("Hello World!", 32, 32)
d.show()

Example for nrf52840 / pca10056:

from machine import Pin, SPI
from display import ILI9341
cs = Pin("B6", mode=Pin.OUT, pull=Pin.PULL_UP)
dc = Pin("B7", mode=Pin.OUT, pull=Pin.PULL_UP)
spi = SPI(0, baudrate=8000000)
d = ILI9341(240, 320, spi, cs, dc)
d.text("Hello World!", 32, 32)
d.show()

*/
STATIC mp_obj_t lcd_ili9341_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    static const mp_arg_t allowed_args[] = {
        { ARG_NEW_WIDTH,  MP_ARG_REQUIRED | MP_ARG_INT },
        { ARG_NEW_HEIGHT, MP_ARG_REQUIRED | MP_ARG_INT },
        { ARG_NEW_SPI,    MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { ARG_NEW_CS,     MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { ARG_NEW_DC,     MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    };

    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    lcd_ili9341_obj_t *s = m_new_obj_with_finaliser(lcd_ili9341_obj_t);
    s->base.type = type;

    mp_int_t width;
    mp_int_t height;

    if (args[ARG_NEW_WIDTH].u_int > 0) {
        width = args[ARG_NEW_WIDTH].u_int;
    } else {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_ValueError,
                  "Display width not set"));
    }

    if (args[ARG_NEW_HEIGHT].u_int > 0) {
        height = args[ARG_NEW_HEIGHT].u_int;
    } else {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_ValueError,
                  "Display height not set"));
    }

    if (args[ARG_NEW_SPI].u_obj != MP_OBJ_NULL) {
        s->spi = args[ARG_NEW_SPI].u_obj;
    } else {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_ValueError,
                  "Display SPI not set"));
    }

    if (args[ARG_NEW_CS].u_obj != MP_OBJ_NULL) {
        s->pin_cs = args[ARG_NEW_CS].u_obj;
    } else {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_ValueError,
                  "Display CS Pin not set"));
    }

    if (args[ARG_NEW_DC].u_obj != MP_OBJ_NULL) {
        s->pin_dc = args[ARG_NEW_DC].u_obj;
    } else {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_ValueError,
                  "Display DC Pin not set"));
    }

    // direction arg not yet configurable
    mp_int_t vertical = true;
    s->framebuffer = lcd_mono_fb_helper_make_new(width, height, vertical);

    driver_ili9341_init(s->spi->pyb->spi->instance, s->pin_cs, s->pin_dc);
    // Default to white background
    driver_ili9341_clear(0xFFFF);

    display_clear_screen(s->framebuffer, 0x1);

    return MP_OBJ_FROM_PTR(s);
}

// text

/// \method fill(color)
/// Fill framebuffer with the color defined as argument.
STATIC mp_obj_t lcd_ili9341_fill(mp_obj_t self_in, mp_obj_t color) {
    lcd_ili9341_obj_t *self = MP_OBJ_TO_PTR(self_in);

    display_clear_screen(self->framebuffer, (uint8_t)mp_obj_get_int(color));

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(lcd_ili9341_fill_obj, lcd_ili9341_fill);

/// \method show()
/// Display content in framebuffer.
STATIC mp_obj_t lcd_ili9341_show(size_t n_args, const mp_obj_t *args) {
    lcd_ili9341_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    display_update(self->framebuffer, false, dirty_line_update_cb);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lcd_ili9341_show_obj, 1, 2, lcd_ili9341_show);

/// \method refresh([num_of_refresh])
/// Refresh content in framebuffer.
///
///   - With no argument, 1 refresh will be done.
///   - With `num_of_refresh` given, The whole framebuffer will be considered
///   dirty and will be refreshed the given number of times.
STATIC mp_obj_t lcd_ili9341_refresh(mp_obj_t self_in) {
    lcd_ili9341_obj_t *self = MP_OBJ_TO_PTR(self_in);

    (void)self;

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(lcd_ili9341_refresh_obj, lcd_ili9341_refresh);

/// \method pixel(x, y, [color])
/// Write one pixel in framebuffer.
///
///   - With no argument, the color of the pixel in framebuffer will be returend.
///   - With `color` given, sets the pixel to the color given.
STATIC mp_obj_t lcd_ili9341_pixel(size_t n_args, const mp_obj_t *args) {
    lcd_ili9341_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t x = mp_obj_get_int(args[1]);
    mp_int_t y = mp_obj_get_int(args[2]);
    mp_int_t color;
    if (n_args >= 3) {
        color = mp_obj_get_int(args[3]);
    }
    (void)self;
    (void)x;
    (void)y;
    (void)color;

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lcd_ili9341_pixel_obj, 3, 4, lcd_ili9341_pixel);

/// \method pixel(text, x, y, [color])
/// Write one pixel in framebuffer.
///
///   - With no argument, the color will be the opposite of background (fill color).
///   - With `color` given, sets the pixel to the color given.
STATIC mp_obj_t lcd_ili9341_text(size_t n_args, const mp_obj_t *args) {
    lcd_ili9341_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const char *str = mp_obj_str_get_str(args[1]);
    mp_int_t x = mp_obj_get_int(args[2]);
    mp_int_t y = mp_obj_get_int(args[3]);
    mp_int_t color;
    if (n_args >= 4) {
        color = mp_obj_get_int(args[3]);
    }

    display_print_string(self->framebuffer, x, y, str);

    (void)color;

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lcd_ili9341_text_obj, 4, 5, lcd_ili9341_text);

STATIC mp_obj_t lcd_ili9341_del(mp_obj_t self_in) {
    lcd_ili9341_obj_t *self = MP_OBJ_TO_PTR(self_in);

    (void)self;

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(lcd_ili9341_del_obj, lcd_ili9341_del);

STATIC const mp_map_elem_t lcd_ili9341_locals_dict_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___del__), (mp_obj_t)(&lcd_ili9341_del_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_fill), (mp_obj_t)(&lcd_ili9341_fill_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_show), (mp_obj_t)(&lcd_ili9341_show_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_text), (mp_obj_t)(&lcd_ili9341_text_obj) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_pixel), (mp_obj_t)(&lcd_ili9341_pixel_obj) },
#if 0
    { MP_OBJ_NEW_QSTR(MP_QSTR_bitmap), (mp_obj_t)(&lcd_ili9341_bitmap_obj) },
#endif
    { MP_OBJ_NEW_QSTR(MP_QSTR_COLOR_BLACK), MP_OBJ_NEW_SMALL_INT(LCD_BLACK) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_COLOR_WHITE), MP_OBJ_NEW_SMALL_INT(LCD_WHITE) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_VERTICAL), MP_OBJ_NEW_SMALL_INT(0) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_HORIZONTAL), MP_OBJ_NEW_SMALL_INT(1) },
};

STATIC MP_DEFINE_CONST_DICT(lcd_ili9341_locals_dict, lcd_ili9341_locals_dict_table);

const mp_obj_type_t lcd_ili9341_type = {
    { &mp_type_type },
    .name = MP_QSTR_ILI9341,
    .print = lcd_ili9341_print,
    .make_new = lcd_ili9341_make_new,
    .locals_dict = (mp_obj_t)&lcd_ili9341_locals_dict
};

#endif // MICROPY_PY_DISPLAY_LCD_ILI9341