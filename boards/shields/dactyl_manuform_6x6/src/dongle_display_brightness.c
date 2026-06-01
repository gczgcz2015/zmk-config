#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>

static const struct device *const pwm_leds_dev = DEVICE_DT_GET_ONE(pwm_leds);

#define DISP_BL DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

static int dongle_display_brightness_init(void) {
    if (!device_is_ready(pwm_leds_dev)) {
        return -ENODEV;
    }

    return led_set_brightness(pwm_leds_dev, DISP_BL,
                              CONFIG_DACTYL_MANUFORM_6X6_DISPLAY_FIXED_BRIGHTNESS);
}

SYS_INIT(dongle_display_brightness_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
