#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

static const struct device *const pwm_leds_dev = DEVICE_DT_GET_ONE(pwm_leds);

#define DISP_BL DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

static int set_display_brightness(uint8_t brightness) {
    if (!device_is_ready(pwm_leds_dev)) {
        return -ENODEV;
    }

    return led_set_brightness(pwm_leds_dev, DISP_BL, brightness);
}

static int dongle_display_brightness_init(void) {
    return set_display_brightness(CONFIG_DACTYL_MANUFORM_6X6_DISPLAY_FIXED_BRIGHTNESS);
}

SYS_INIT(dongle_display_brightness_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int dongle_display_brightness_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    switch (ev->state) {
    case ZMK_ACTIVITY_ACTIVE:
        set_display_brightness(CONFIG_DACTYL_MANUFORM_6X6_DISPLAY_FIXED_BRIGHTNESS);
        break;
    case ZMK_ACTIVITY_IDLE:
    case ZMK_ACTIVITY_SLEEP:
        set_display_brightness(0);
        break;
    default:
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dongle_display_brightness, dongle_display_brightness_activity_listener);
ZMK_SUBSCRIPTION(dongle_display_brightness, zmk_activity_state_changed);
