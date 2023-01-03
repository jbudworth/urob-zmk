
#pragma once

#include <bluetooth/addr.h>
#include <zmk/behavior.h>

int zmk_split_bt_invoke_behavior(uint8_t source, struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event, bool state);

int zmk_split_bt_update_led(struct zmk_periph_led *periph);
int zmk_split_bt_update_bl(struct backlight_state *periph);