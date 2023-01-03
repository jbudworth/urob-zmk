/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#define ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN 9

struct zmk_split_run_behavior_data {
    uint8_t position;
    uint8_t state;
    uint32_t param1;
    uint32_t param2;
} __packed;

struct zmk_split_run_behavior_payload {
    struct zmk_split_run_behavior_data data;
    char behavior_dev[ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN];
} __packed;

struct zmk_split_update_led_data {
    uint8_t layer;
    uint8_t indicators;
} __packed;

struct zmk_split_update_bl_data {
    uint8_t brightness;
    bool on;
} __packed;

int zmk_split_bt_position_pressed(uint8_t position);
int zmk_split_bt_position_released(uint8_t position);