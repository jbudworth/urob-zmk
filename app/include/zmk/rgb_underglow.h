/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/led_indicators_types.h>

struct zmk_led_hsb {
    uint16_t h;
    uint8_t s;
    uint8_t b;
};

struct zmk_periph_led {
    uint8_t layer;
    zmk_leds_flags_t indicators;
};

int zmk_rgb_underglow_toggle();
int zmk_rgb_underglow_get_state(bool *state);
int zmk_rgb_underglow_on();
int zmk_rgb_underglow_off();
int zmk_rgb_underglow_cycle_effect(int direction);
int zmk_rgb_underglow_calc_effect(int direction);
int zmk_rgb_underglow_select_effect(int effect);
int zmk_rgb_underglow_set_periph(struct zmk_periph_led periph);
struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction);
struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction);
struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction);
int zmk_rgb_underglow_change_hue(int direction);
int zmk_rgb_underglow_change_sat(int direction);
int zmk_rgb_underglow_change_brt(int direction);
int zmk_rgb_underglow_change_spd(int direction);
int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color);