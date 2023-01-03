/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <device.h>
#include <init.h>
#include <kernel.h>
#include <settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <logging/log.h>

#include <drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/rgb_underglow.h>

#include <zmk/activity.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/led_indicator_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/led_indicators.h>
#include <zmk/battery.h>
#include <zmk/keymap.h>
#include <zmk/ble.h>

#if ZMK_BLE_IS_CENTRAL
#include <zmk/split/bluetooth/central.h>
#else
#include <zmk/split/bluetooth/peripheral.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define STRIP_LABEL DT_LABEL(DT_CHOSEN(zmk_underglow))
#define STRIP_NUM_PIXELS DT_PROP(DT_CHOSEN(zmk_underglow), chain_length)

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

BUILD_ASSERT(CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

enum rgb_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
    UNDERGLOW_EFFECT_KINESIS,
    UNDERGLOW_EFFECT_BATTERY,
    UNDERGLOW_EFFECT_TEST,
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct rgb_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
};

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];

static struct rgb_underglow_state state;

static struct zmk_periph_led led_data;

static bool last_ble_state[2];

static bool triggered;

#if ZMK_BLE_IS_CENTRAL
static struct zmk_periph_led old_led_data;
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *ext_power;
#endif

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX / BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r, g, b;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}

int zmk_rgb_underglow_set_periph(struct zmk_periph_led periph) {
    led_data = periph;
    LOG_DBG("Update led_data %d %d", led_data.layer, led_data.indicators);
    return 0;
}

static void zmk_rgb_underglow_effect_solid() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(state.color));
    }
}

static void zmk_rgb_underglow_effect_breathe() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.b = abs(state.animation_step - 1200) / 12;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step += state.animation_speed * 10;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_rgb_underglow_effect_spectrum() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_rgb_underglow_effect_swirl() {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % HUE_MAX;
}

#if ZMK_BLE_IS_CENTRAL
static struct k_work_delayable led_update_work;

static void zmk_rgb_underglow_central_send() {
    int err = zmk_split_bt_update_led(&led_data);
    if (err) {
        LOG_ERR("send failed (err %d)", err);
    }
}
#endif

static void zmk_rgb_underglow_effect_kinesis() {
#if ZMK_BLE_IS_CENTRAL
    // leds for central(left) side

    old_led_data.layer = led_data.layer;
    old_led_data.indicators = led_data.indicators;
    led_data.indicators = zmk_leds_get_current_flags();
    led_data.layer = zmk_keymap_highest_layer_active();

    pixels[0].r = (led_data.indicators & ZMK_LED_CAPSLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
    pixels[0].g = (led_data.indicators & ZMK_LED_CAPSLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
    pixels[0].b = (led_data.indicators & ZMK_LED_CAPSLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
    // set second led as bluetooth state
    switch (zmk_ble_active_profile_index()) {
    case 0:
        pixels[1].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[1].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[1].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        break;
    case 1:
        pixels[1].r = 0;
        pixels[1].g = 0;
        pixels[1].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        break;
    case 2:
        pixels[1].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[1].g = 0;
        pixels[1].b = 0;
        break;
    case 3:
        pixels[1].r = 0;
        pixels[1].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[1].b = 0;
        break;
    }
    // blink second led slowly if bluetooth not paired, quickly if not connected
    if (zmk_ble_active_profile_is_open()) {
        pixels[1].r = pixels[1].r * last_ble_state[0];
        pixels[1].g = pixels[1].g * last_ble_state[0];
        pixels[1].b = pixels[1].b * last_ble_state[0];
        if (state.animation_step > 3) {
            last_ble_state[0] = !last_ble_state[0];
            state.animation_step = 0;
        }
        state.animation_step++;
    } else if (!zmk_ble_active_profile_is_connected()) {
        pixels[1].r = pixels[1].r * last_ble_state[1];
        pixels[1].g = pixels[1].g * last_ble_state[1];
        pixels[1].b = pixels[1].b * last_ble_state[1];
        if (state.animation_step > 14) {
            last_ble_state[1] = !last_ble_state[1];
            state.animation_step = 0;
        }
        state.animation_step++;
    }
    // set third led as layer state
    switch (led_data.layer) {
    case 0:
        pixels[2].r = 0;
        pixels[2].g = 0;
        pixels[2].b = 0;
        break;
    case 1:
        pixels[2].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        break;
    case 2:
        pixels[2].r = 0;
        pixels[2].g = 0;
        pixels[2].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        break;
    case 3:
        pixels[2].r = 0;
        pixels[2].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].b = 0;
        break;
    case 4:
        pixels[2].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].g = 0;
        pixels[2].b = 0;
        break;
    case 5:
        pixels[2].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].g = 0;
        pixels[2].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        break;
    case 6:
        pixels[2].r = 0;
        pixels[2].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        break;
    case 7:
        pixels[2].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].b = 0;
        break;
    default:
        pixels[2].r = 0;
        pixels[2].g = 0;
        pixels[2].b = 0;
        break;
    }
    if (old_led_data.layer != led_data.layer || old_led_data.indicators != led_data.indicators) {
        zmk_rgb_underglow_central_send();
    }
#else
    // leds for peripheral(right) side
    /* if (zmk_ble_active_profile_is_open()) {
        pixels[0].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE * last_ble_state[0];
        pixels[0].g = 0;
        pixels[0].b = 0;
        pixels[1].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE * last_ble_state[0];
        pixels[1].g = 0;
        pixels[1].b = 0;
        pixels[2].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE * last_ble_state[0];
        pixels[2].g = 0;
        pixels[2].b = 0;
        if (state.animation_step > 3) {
            last_ble_state[0] = !last_ble_state[0];
            state.animation_step = 0;
        }
        state.animation_step++;
    } else */
    if (!zmk_split_bt_peripheral_is_connected()) {
        pixels[0].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE * last_ble_state[1];
        pixels[0].g = 0;
        pixels[0].b = 0;
        pixels[1].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE * last_ble_state[1];
        pixels[1].g = 0;
        pixels[1].b = 0;
        pixels[2].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE * last_ble_state[1];
        pixels[2].g = 0;
        pixels[2].b = 0;
        if (state.animation_step > 14) {
            last_ble_state[1] = !last_ble_state[1];
            state.animation_step = 0;
        }
        state.animation_step++;
    } else {
        // set first led as LED_NUMLOCK
        pixels[2].r =
            (led_data.indicators & ZMK_LED_NUMLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].g =
            (led_data.indicators & ZMK_LED_NUMLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[2].b =
            (led_data.indicators & ZMK_LED_NUMLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        // set second led as scroll Lock
        pixels[1].r =
            (led_data.indicators & ZMK_LED_SCROLLLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[1].g =
            (led_data.indicators & ZMK_LED_SCROLLLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        pixels[1].b =
            (led_data.indicators & ZMK_LED_SCROLLLOCK_BIT) * CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
        // set third led as layer
        switch (led_data.layer) {
        case 0:
            pixels[0].r = 0;
            pixels[0].g = 0;
            pixels[0].b = 0;
            break;
        case 1:
            pixels[0].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            pixels[0].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            pixels[0].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            break;
        case 2:
            pixels[0].r = 0;
            pixels[0].g = 0;
            pixels[0].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            break;
        case 3:
            pixels[0].r = 0;
            pixels[0].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            pixels[0].b = 0;
            break;
        case 4:
            pixels[0].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            pixels[0].g = 0;
            pixels[0].b = 0;
            break;
        case 5:
            pixels[0].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            pixels[0].g = 0;
            pixels[0].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            break;
        case 6:
            pixels[0].r = 0;
            pixels[0].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            pixels[0].b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            break;
        case 7:
            pixels[0].r = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            pixels[0].g = CONFIG_ZMK_RGB_UNDERGLOW_BRT_SCALE;
            pixels[0].b = 0;
            break;
        default:
            pixels[0].r = 0;
            pixels[0].g = 0;
            pixels[0].b = 0;
            break;
        }
    }
#endif
}

static void zmk_rgb_underglow_effect_test() {
    triggered = true;
    struct led_rgb rgb;
    rgb.r = 0;
    rgb.g = 0;
    rgb.b = 0;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }
    if (state.animation_step < (HUE_MAX * 3)) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;
        rgb.r = 0;

        pixels[0] = rgb;
        pixels[1] = rgb;
        pixels[2] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }
    if (state.animation_step < (HUE_MAX * 2)) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step - HUE_MAX;
        rgb.r = 0;
        rgb.g = 0;
        rgb.b = 0;
        pixels[0] = rgb;
        pixels[1] = hsb_to_rgb(hsb_scale_min_max(hsb));
        pixels[2] = rgb;
    }
    if (state.animation_step < HUE_MAX) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;
        rgb.r = 0;
        rgb.g = 0;
        rgb.b = 0;
        pixels[0] = hsb_to_rgb(hsb_scale_min_max(hsb));
        pixels[1] = rgb;
        pixels[2] = rgb;
    }

    state.animation_step += 20;
    if (state.animation_step > (HUE_MAX * 3)) {

        rgb.r = 255;
        rgb.g = 255;
        rgb.b = 255;
        for (int i = 0; i < STRIP_NUM_PIXELS; i++)
            pixels[i] = rgb;
    }
}

static void zmk_rgb_underglow_effect_battery() {
    uint8_t soc = zmk_battery_state_of_charge();
    struct led_rgb rgb;
    if (soc > 80) {
        rgb.r = 0;
        rgb.g = 255;
        rgb.b = 0;
    } else if (soc > 50 && soc < 80) {
        rgb.r = 255;
        rgb.g = 255;
        rgb.b = 0;
    } else if (soc > 20 && soc < 51) {
        rgb.r = 255;
        rgb.g = 140;
        rgb.b = 0;
    } else {
        rgb.r = 255;
        rgb.g = 0;
        rgb.b = 0;
    }
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
}

static void zmk_rgb_underglow_tick(struct k_work *work) {
    switch (state.current_effect) {
    case UNDERGLOW_EFFECT_SOLID:
        zmk_rgb_underglow_effect_solid();
        break;
    case UNDERGLOW_EFFECT_BREATHE:
        zmk_rgb_underglow_effect_breathe();
        break;
    case UNDERGLOW_EFFECT_SPECTRUM:
        zmk_rgb_underglow_effect_spectrum();
        break;
    case UNDERGLOW_EFFECT_SWIRL:
        zmk_rgb_underglow_effect_swirl();
        break;
    case UNDERGLOW_EFFECT_KINESIS:
        zmk_rgb_underglow_effect_kinesis();
        break;
    case UNDERGLOW_EFFECT_BATTERY:
        zmk_rgb_underglow_effect_battery();
        break;
    case UNDERGLOW_EFFECT_TEST:
        zmk_rgb_underglow_effect_test();
        break;
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

K_WORK_DEFINE(underglow_work, zmk_rgb_underglow_tick);

static void zmk_rgb_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }

    k_work_submit(&underglow_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_rgb_underglow_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

struct settings_handler rgb_conf = {.name = "rgb/underglow", .h_set = rgb_settings_set};

static void zmk_rgb_underglow_save_state_work() {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_work_delayable underglow_save_work;
#endif

int zmk_rgb_underglow_save_state() {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

static int zmk_rgb_underglow_init(const struct device *_arg) {
    led_strip = device_get_binding(STRIP_LABEL);
    if (led_strip) {
        LOG_INF("Found LED strip device %s", STRIP_LABEL);
    } else {
        LOG_ERR("LED strip device %s not found", STRIP_LABEL);
        return -EINVAL;
    }

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    ext_power = device_get_binding("EXT_POWER");
    if (ext_power == NULL) {
        LOG_ERR("Unable to retrieve ext_power device: EXT_POWER");
    }
#endif

    state = (struct rgb_underglow_state){
        color : {
            h : CONFIG_ZMK_RGB_UNDERGLOW_HUE_START,
            s : CONFIG_ZMK_RGB_UNDERGLOW_SAT_START,
            b : CONFIG_ZMK_RGB_UNDERGLOW_BRT_START,
        },
        animation_speed : CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        current_effect : CONFIG_ZMK_RGB_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();

    int err = settings_register(&rgb_conf);
    if (err) {
        LOG_ERR("Failed to register the ext_power settings handler (err %d)", err);
        return err;
    }
    led_data.indicators = 0;
    k_work_init_delayable(&underglow_save_work, zmk_rgb_underglow_save_state_work);

    settings_load_subtree("rgb/underglow");
#endif

#if ZMK_BLE_IS_CENTRAL
    k_work_init_delayable(&led_update_work, zmk_rgb_underglow_central_send);
#endif

    zmk_rgb_underglow_save_state();
    k_work_submit(&underglow_work);
    zmk_rgb_underglow_off();
    zmk_rgb_underglow_on();
    triggered = false;
    return 0;
}

int zmk_rgb_underglow_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = state.on;
    return 0;
}

int zmk_rgb_underglow_on() {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }
#endif

    state.on = true;
    state.animation_step = 0;
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_off() {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);

    k_timer_stop(&underglow_tick);
    state.on = false;

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_calc_effect(int direction) {
    return (state.current_effect + UNDERGLOW_EFFECT_NUMBER + direction) % UNDERGLOW_EFFECT_NUMBER;
}

int zmk_rgb_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= UNDERGLOW_EFFECT_NUMBER) {
        return -EINVAL;
    }

    state.current_effect = effect;
    state.animation_step = 0;

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    return zmk_rgb_underglow_select_effect(zmk_rgb_underglow_calc_effect(direction));
}

int zmk_rgb_underglow_toggle() {
    return state.on ? zmk_rgb_underglow_off() : zmk_rgb_underglow_on();
}

int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    state.color = color;

    return 0;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.color;

    color.h += HUE_MAX + (direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.color;

    int s = color.s + (direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP);
    if (s < 0) {
        s = 0;
    } else if (s > SAT_MAX) {
        s = SAT_MAX;
    }
    color.s = s;

    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction) {
    struct zmk_led_hsb color = state.color;

    int b = color.b + (direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}

int zmk_rgb_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_hue(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_sat(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_rgb_underglow_calc_brt(direction);

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }

    state.animation_speed += direction;

    if (state.animation_speed > 5) {
        state.animation_speed = 5;
    }

    return zmk_rgb_underglow_save_state();
}

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
static int rgb_underglow_auto_state(bool *prev_state, bool new_state) {
    if (state.on == new_state) {
        return 0;
    }
    state.on = new_state && *prev_state;
    *prev_state = !new_state;
    if (state.on)
        return zmk_rgb_underglow_on();
    else
        return zmk_rgb_underglow_off();
}

#endif
static int rgb_underglow_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        static bool prev_state = false;
        return rgb_underglow_auto_state(&prev_state,
                                        zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        led_data.indicators = zmk_led_indicators_get_current_flags();
        led_data.layer = zmk_keymap_highest_layer_active();
        int err = zmk_split_bt_update_led(&led_data);
        if (err) {
            LOG_ERR("send failed (err %d)", err);
        }
        static bool prev_state = false;
        return rgb_underglow_auto_state(&prev_state, zmk_usb_is_powered());
    }
#endif

#if ZMK_BLE_IS_CENTRAL
    if (as_zmk_split_peripheral_status_changed(eh)) {
        LOG_DBG("event called");
        const struct zmk_split_peripheral_status_changed *ev;
        ev = as_zmk_split_peripheral_status_changed(eh);
        if (ev->connected) {
            k_work_reschedule(&led_update_work, K_MSEC(2500));
            return 0;
        } else {
            k_work_cancel_delayable(&led_update_work);
            return 0;
        }
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_underglow, rgb_underglow_event_listener);
// IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_usb_conn_state_changed);
#endif

#if ZMK_BLE_IS_CENTRAL
ZMK_SUBSCRIPTION(rgb_underglow, zmk_split_peripheral_status_changed);
#endif

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
