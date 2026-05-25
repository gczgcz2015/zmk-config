#include <lvgl.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "bongo_cat_art.h"

#define BONGO_REST_DELAY_MS 160

enum bongo_cat_frame {
    BONGO_CAT_RESTING,
    BONGO_CAT_LEFT,
    BONGO_CAT_RIGHT,
};

static struct k_work_delayable bongo_frame_work;
static struct k_work_delayable display_overlay_work;
static lv_obj_t *bongo_cat_img;
static bool display_overlay_installed;
static enum bongo_cat_frame pending_bongo_frame = BONGO_CAT_RESTING;
static uint8_t active_key_count;
static bool use_left_frame = true;

static const lv_img_dsc_t *bongo_frame_image(enum bongo_cat_frame frame) {
    switch (frame) {
    case BONGO_CAT_LEFT:
        return &bongo_casualleft;
    case BONGO_CAT_RIGHT:
        return &bongo_casualright;
    case BONGO_CAT_RESTING:
    default:
        return &bongo_resting;
    }
}

static void apply_bongo_frame(void *unused) {
    ARG_UNUSED(unused);

    if (bongo_cat_img == NULL) {
        return;
    }

    lv_img_set_src(bongo_cat_img, bongo_frame_image(pending_bongo_frame));
}

static void bongo_frame_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lv_async_call(apply_bongo_frame, NULL);
}

static void schedule_bongo_frame(enum bongo_cat_frame frame, k_timeout_t delay) {
    pending_bongo_frame = frame;
    k_work_reschedule(&bongo_frame_work, delay);
}

static void hide_unavailable_battery_labels(lv_obj_t *obj) {
    if (obj == NULL) {
        return;
    }

    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj);
        if (text != NULL && strcmp(text, "N/A") == 0) {
            lv_obj_set_style_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
        }
    }

    uint32_t child_count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        hide_unavailable_battery_labels(lv_obj_get_child(obj, i));
    }
}

static void shrink_layer_roller(lv_obj_t *screen) {
    uint32_t child_count = lv_obj_get_child_cnt(screen);

    if (child_count == 0) {
        return;
    }

    lv_obj_t *layer_roller = lv_obj_get_child(screen, child_count - 1);
    lv_obj_set_size(layer_roller, 112, 64);
    lv_obj_align(layer_roller, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_text_font(layer_roller, LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_font(layer_roller, LV_FONT_DEFAULT, LV_PART_SELECTED);
}

static void create_bongo_cat(lv_obj_t *screen) {
    lv_obj_t *cat = lv_obj_create(screen);
    lv_obj_set_size(cat, 204, 120);
    lv_obj_align(cat, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_bg_opa(cat, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cat, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cat, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cat, LV_OBJ_FLAG_SCROLLABLE);

    bongo_cat_img = lv_img_create(cat);
    lv_img_set_src(bongo_cat_img, &bongo_resting);
    lv_obj_center(bongo_cat_img);
}

static void install_display_overlay(void *unused) {
    ARG_UNUSED(unused);

    if (display_overlay_installed) {
        return;
    }

    lv_obj_t *screen = lv_scr_act();
    if (screen == NULL || lv_obj_get_child_cnt(screen) == 0) {
        k_work_reschedule(&display_overlay_work, K_MSEC(500));
        return;
    }

    shrink_layer_roller(screen);
    hide_unavailable_battery_labels(screen);
    create_bongo_cat(screen);
    display_overlay_installed = true;
}

static void display_overlay_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lv_async_call(install_display_overlay, NULL);
}

static int display_overlay_init(void) {
    k_work_init_delayable(&bongo_frame_work, bongo_frame_work_handler);
    k_work_init_delayable(&display_overlay_work, display_overlay_work_handler);
    k_work_schedule(&display_overlay_work, K_SECONDS(2));

    return 0;
}

SYS_INIT(display_overlay_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int bongo_cat_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        if (active_key_count < UINT8_MAX) {
            active_key_count++;
        }

        schedule_bongo_frame(use_left_frame ? BONGO_CAT_LEFT : BONGO_CAT_RIGHT, K_NO_WAIT);
        use_left_frame = !use_left_frame;
    } else {
        if (active_key_count > 0) {
            active_key_count--;
        }

        if (active_key_count == 0) {
            schedule_bongo_frame(BONGO_CAT_RESTING, K_MSEC(BONGO_REST_DELAY_MS));
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dactyl_bongo_cat, bongo_cat_listener);
ZMK_SUBSCRIPTION(dactyl_bongo_cat, zmk_position_state_changed);
