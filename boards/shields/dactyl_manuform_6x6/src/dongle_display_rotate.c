#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/init.h>

static int dongle_display_rotate_init(void) {
    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(display)) {
        return -ENODEV;
    }

    return display_set_orientation(display, DISPLAY_ORIENTATION_ROTATED_270);
}

SYS_INIT(dongle_display_rotate_init, APPLICATION, 60);
