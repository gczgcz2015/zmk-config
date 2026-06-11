#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

#include "bongo_cat_art.h"

#ifdef CONFIG_ZMK_CAPS_WORD
#include <zmk/events/caps_word_state_changed.h>
#endif

LV_FONT_DECLARE(silkscreen_bold_16);
LV_FONT_DECLARE(silkscreen_regular_14);
LV_FONT_DECLARE(silkscreen_regular_16);

/* ──────────────────────── Bongo Cat Settings ──────────────────────── */

#define BONGO_ACTIVE_MS     110
#define BONGO_DOWN_MS       80
#define BONGO_BUSY_TICK_MS  120
#define BONGO_BUSY_KPS_X10  45

enum bongo_cat_frame {
    BONGO_CAT_RESTING,
    BONGO_CAT_LEFT_UP,
    BONGO_CAT_LEFT_DOWN,
    BONGO_CAT_RIGHT_UP,
    BONGO_CAT_RIGHT_DOWN,
    BONGO_CAT_BUSY,
    BONGO_CAT_BOTH,
};

/* ──────────────────────── Typing Speed Tracker ──────────────────────── */

#define SPEED_RING_SIZE     32    /* Track last 32 keystrokes             */
#define SPEED_WINDOW_MS     1200  /* Sliding time window in ms            */

/*
 * The cat image is 204x120. Use a full-screen transparent container so
 * offsets cannot clip the larger fixed frame.
 */
#define CAT_CONTAINER_W     240
#define CAT_CONTAINER_H     135
#define CAT_X_OFFSET        0
#define CAT_Y_OFFSET        8
#define CAT_CONTAINER_X     0
#define CAT_CONTAINER_Y     -12
#define CAT_IMAGE_W         204
#define CAT_IMAGE_H         120
#define BONGO_RIGHT_FIRST_POSITION 31
#define LEFT_TAP_MASK_X     49
#define LEFT_TAP_MASK_Y     80
#define LEFT_TAP_MASK_W     25
#define LEFT_TAP_MASK_H     13
#define RIGHT_TAP_MASK_X    109
#define RIGHT_TAP_MASK_Y    89
#define RIGHT_TAP_MASK_W    31
#define RIGHT_TAP_MASK_H    12

/* ──────────────────────── Modifier Status ──────────────────────── */

#define MOD_STATUS_TICK_MS  100
#define MOD_STATUS_W        232
#define MOD_STATUS_BOTTOM_Y -45
#define MOD_STATUS_SPACING  2

// 13x13 custom pixel-art modifier icons
static const uint8_t caps_symbol_map[] = {
    0x02, 0x00, 0x07, 0x00, 0x0F, 0x80, 0x1F, 0xC0,
    0x3F, 0xE0, 0x7F, 0xF0, 0x1F, 0xC0, 0x00, 0x00,
    0x7F, 0xF0, 0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
};
static const lv_img_dsc_t caps_symbol_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = 13,
    .header.h = 13,
    .data_size = sizeof(caps_symbol_map),
    .data = caps_symbol_map,
};

static const uint8_t ctrl_symbol_map[] = {
    0x02, 0x00, 0x07, 0x00, 0x0D, 0x80, 0x18, 0xC0,
    0x30, 0x60, 0x60, 0x30, 0xFF, 0xF8, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
};
static const lv_img_dsc_t ctrl_symbol_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = 13,
    .header.h = 13,
    .data_size = sizeof(ctrl_symbol_map),
    .data = ctrl_symbol_map,
};

static const uint8_t shift_symbol_map[] = {
    0x02, 0x00, 0x07, 0x00, 0x0F, 0x80, 0x1F, 0xC0,
    0x3F, 0xE0, 0x7F, 0xF0, 0x1F, 0xC0, 0x1F, 0xC0,
    0x1F, 0xC0, 0x1F, 0xC0, 0x1F, 0xC0, 0x1F, 0xC0,
    0x00, 0x00
};
static const lv_img_dsc_t shift_symbol_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = 13,
    .header.h = 13,
    .data_size = sizeof(shift_symbol_map),
    .data = shift_symbol_map,
};

static const uint8_t alt_symbol_map[] = {
    0x00, 0x00, 0xF0, 0x00, 0x1C, 0x00, 0x07, 0x00,
    0x01, 0xC0, 0x00, 0x78, 0x00, 0x00, 0x7F, 0xF0,
    0x7F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
};
static const lv_img_dsc_t alt_symbol_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = 13,
    .header.h = 13,
    .data_size = sizeof(alt_symbol_map),
    .data = alt_symbol_map,
};

static const uint8_t win_symbol_map[] = {
    0x00, 0x00, 0x38, 0xE0, 0x6D, 0xB0, 0x6D, 0xB0,
    0x3F, 0xE0, 0x08, 0x80, 0x3F, 0xE0, 0x6D, 0xB0,
    0x6D, 0xB0, 0x38, 0xE0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00
};
static const lv_img_dsc_t win_symbol_img = {
    .header.cf = LV_IMG_CF_ALPHA_1BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = 13,
    .header.h = 13,
    .data_size = sizeof(win_symbol_map),
    .data = win_symbol_map,
};

/* ──────────────────────── Layer Status ──────────────────────── */

#define LAYER_STATUS_W      92
#define LAYER_FN_INDEX      1

/* ──────────────────────── Battery Status ──────────────────────── */

#define BATTERY_STATUS_H     48
#define BATTERY_BAR_H        4
#define BATTERY_ROW_PAD_X    16
#define BATTERY_ROW_PAD_B    12
#define BATTERY_ROW_GAP      12
#define BATTERY_SLOT_COUNT   CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS

/* ──────────────────────── Static Variables ──────────────────────── */

/* Bongo cat */
static struct k_work_delayable bongo_frame_work;
static struct k_work_delayable bongo_down_work;
static struct k_work_delayable bongo_return_work;
static struct k_work_delayable bongo_busy_work;
static struct k_work_delayable modifier_status_work;
static lv_obj_t *bongo_cat_img;
static lv_obj_t *cat_container;
static lv_obj_t *left_tap_mask;
static lv_obj_t *right_tap_mask;
static lv_obj_t *base_layer_badge;
static lv_obj_t *base_layer_label;
static lv_obj_t *fn_layer_badge;
static lv_obj_t *fn_layer_label;
static lv_obj_t *modifier_status_row;
static lv_obj_t *mod_boxes[5];
static bool display_screen_ready;
static bool display_work_ready;
static enum bongo_cat_frame pending_bongo_frame = BONGO_CAT_RESTING;
static uint8_t active_key_count;
static bool busy_tick_running;
static uint8_t busy_phase;
static enum bongo_cat_frame pending_down_frame = BONGO_CAT_LEFT_DOWN;
static uint8_t active_modifier_counts[8];
static uint8_t position_modifier_mask;
static uint8_t pending_modifier_mask;
#ifdef CONFIG_ZMK_CAPS_WORD
static bool caps_word_active;
#endif

/* Typing speed ring buffer (accessed from both event and LVGL contexts) */
static int64_t keystroke_times[SPEED_RING_SIZE];
static uint8_t speed_ring_head;
static uint8_t speed_ring_count;
static struct k_spinlock speed_lock;

struct battery_slot_obj {
    lv_obj_t *icon_body;
    lv_obj_t *icon_cap;
    lv_obj_t *bar;
    lv_obj_t *num;
};

static lv_obj_t *battery_status_row;
static struct battery_slot_obj battery_slots[BATTERY_SLOT_COUNT];
static uint8_t battery_levels[BATTERY_SLOT_COUNT];
static bool battery_level_known[BATTERY_SLOT_COUNT];
static bool battery_connected[BATTERY_SLOT_COUNT];
static struct k_spinlock battery_lock;
static lv_obj_t *wps_label;

/* ══════════════════════════════════════════════════════════════════ */
/*                       Bongo Cat Animation                        */
/* ══════════════════════════════════════════════════════════════════ */

static int calc_kps_x10(void);

static const lv_img_dsc_t *bongo_frame_image(enum bongo_cat_frame frame) {
    switch (frame) {
    case BONGO_CAT_LEFT_UP:
        return &bongo_casualright;
    case BONGO_CAT_LEFT_DOWN:
    case BONGO_CAT_RIGHT_DOWN:
        return &bongo_both;
    case BONGO_CAT_RIGHT_UP:
        return &bongo_casualleft;
    case BONGO_CAT_BUSY:
        return &bongo_busy;
    case BONGO_CAT_BOTH:
        return &bongo_both;
    case BONGO_CAT_RESTING:
    default:
        return &bongo_resting;
    }
}

static void bongo_frame_offset(enum bongo_cat_frame frame, int16_t *x, int16_t *y) {
    *x = CAT_X_OFFSET;
    *y = CAT_Y_OFFSET;

    switch (frame) {
    case BONGO_CAT_LEFT_UP:
        *x -= 4;
        *y += 1;
        break;
    case BONGO_CAT_RIGHT_UP:
        *y -= 1;
        break;
    case BONGO_CAT_BUSY:
        *x -= 5;
        *y -= 2;
        break;
    case BONGO_CAT_LEFT_DOWN:
    case BONGO_CAT_RIGHT_DOWN:
    case BONGO_CAT_BOTH:
    case BONGO_CAT_RESTING:
    default:
        break;
    }
}

static void set_tap_mask(lv_obj_t *mask, int16_t img_x, int16_t img_y,
                         int16_t mask_x, int16_t mask_y,
                         int16_t mask_w, int16_t mask_h) {
    if (mask == NULL) {
        return;
    }

    lv_obj_set_size(mask, mask_w, mask_h);
    lv_obj_set_pos(mask, img_x + mask_x, img_y + mask_y);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(mask);
}

static void apply_tap_masks(enum bongo_cat_frame frame, int16_t frame_x,
                            int16_t frame_y) {
    if (left_tap_mask != NULL) {
        lv_obj_add_flag(left_tap_mask, LV_OBJ_FLAG_HIDDEN);
    }
    if (right_tap_mask != NULL) {
        lv_obj_add_flag(right_tap_mask, LV_OBJ_FLAG_HIDDEN);
    }

    int16_t img_x = (CAT_CONTAINER_W - CAT_IMAGE_W) / 2 + frame_x;
    int16_t img_y = (CAT_CONTAINER_H - CAT_IMAGE_H) / 2 + frame_y;

    switch (frame) {
    case BONGO_CAT_LEFT_UP:
    case BONGO_CAT_LEFT_DOWN:
        set_tap_mask(right_tap_mask, img_x, img_y, RIGHT_TAP_MASK_X,
                     RIGHT_TAP_MASK_Y, RIGHT_TAP_MASK_W, RIGHT_TAP_MASK_H);
        break;
    case BONGO_CAT_RIGHT_UP:
    case BONGO_CAT_RIGHT_DOWN:
        set_tap_mask(left_tap_mask, img_x, img_y, LEFT_TAP_MASK_X,
                     LEFT_TAP_MASK_Y, LEFT_TAP_MASK_W, LEFT_TAP_MASK_H);
        break;
    case BONGO_CAT_RESTING:
    case BONGO_CAT_BUSY:
    case BONGO_CAT_BOTH:
    default:
        break;
    }
}

static void apply_bongo_frame(void *unused) {
    ARG_UNUSED(unused);

    if (bongo_cat_img == NULL) {
        return;
    }

    lv_img_set_src(bongo_cat_img, bongo_frame_image(pending_bongo_frame));

    int16_t x;
    int16_t y;
    bongo_frame_offset(pending_bongo_frame, &x, &y);
    lv_obj_align(bongo_cat_img, LV_ALIGN_CENTER, x, y);
    apply_tap_masks(pending_bongo_frame, x, y);
}

static void bongo_frame_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lv_async_call(apply_bongo_frame, NULL);
}

static void schedule_bongo_frame(enum bongo_cat_frame frame, k_timeout_t delay) {
    if (!display_work_ready) {
        return;
    }

    pending_bongo_frame = frame;
    k_work_reschedule(&bongo_frame_work, delay);
}

static void bongo_return_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!busy_tick_running) {
        schedule_bongo_frame(BONGO_CAT_RESTING, K_NO_WAIT);
    }
}

static void bongo_down_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!busy_tick_running) {
        schedule_bongo_frame(pending_down_frame, K_NO_WAIT);
        k_work_reschedule(&bongo_return_work, K_MSEC(BONGO_ACTIVE_MS));
    }
}

static void bongo_busy_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!display_screen_ready || calc_kps_x10() < BONGO_BUSY_KPS_X10) {
        busy_tick_running = false;
        busy_phase = 0;
        schedule_bongo_frame(BONGO_CAT_RESTING, K_NO_WAIT);
        return;
    }

    enum bongo_cat_frame frame = BONGO_CAT_RESTING;
    if (busy_phase == 1) {
        frame = BONGO_CAT_BUSY;
    } else if (busy_phase == 2) {
        frame = BONGO_CAT_BOTH;
    }

    schedule_bongo_frame(frame, K_NO_WAIT);
    busy_phase = (busy_phase + 1) % 3;
    k_work_reschedule(&bongo_busy_work, K_MSEC(BONGO_BUSY_TICK_MS));
}

static void start_busy_animation(void) {
    if (!display_work_ready) {
        return;
    }

    k_work_cancel_delayable(&bongo_down_work);
    k_work_cancel_delayable(&bongo_return_work);

    if (busy_tick_running) {
        return;
    }

    busy_tick_running = true;
    busy_phase = 0;
    k_work_reschedule(&bongo_busy_work, K_NO_WAIT);
}

static void stop_busy_animation(void) {
    if (!display_work_ready) {
        return;
    }

    busy_tick_running = false;
    busy_phase = 0;
    k_work_cancel_delayable(&bongo_busy_work);
}

static void trigger_typing_frame(bool left_hand) {
    if (!display_work_ready) {
        return;
    }

    pending_down_frame = left_hand ? BONGO_CAT_LEFT_DOWN : BONGO_CAT_RIGHT_DOWN;

    k_work_cancel_delayable(&bongo_down_work);
    k_work_cancel_delayable(&bongo_return_work);
    schedule_bongo_frame(left_hand ? BONGO_CAT_LEFT_UP : BONGO_CAT_RIGHT_UP,
                         K_NO_WAIT);
    k_work_reschedule(&bongo_down_work, K_MSEC(BONGO_DOWN_MS));
}

/* ══════════════════════════════════════════════════════════════════ */
/*                       Typing Speed Tracker                       */
/* ══════════════════════════════════════════════════════════════════ */

static void record_keystroke_time(void) {
    k_spinlock_key_t key = k_spin_lock(&speed_lock);
    keystroke_times[speed_ring_head] = k_uptime_get();
    speed_ring_head = (speed_ring_head + 1) % SPEED_RING_SIZE;
    if (speed_ring_count < SPEED_RING_SIZE) {
        speed_ring_count++;
    }
    k_spin_unlock(&speed_lock, key);
}

/*
 * Returns keys-per-second x 10  (fixed-point to avoid float).
 * E.g. a return value of 45 means 4.5 KPS.
 */
static int calc_kps_x10(void) {
    int64_t now = k_uptime_get();
    int64_t limit = now - SPEED_WINDOW_MS;

    k_spinlock_key_t key = k_spin_lock(&speed_lock);
    uint8_t count = speed_ring_count;
    uint8_t head  = speed_ring_head;
    int active_clicks = 0;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t idx = (head - 1 - i + SPEED_RING_SIZE) % SPEED_RING_SIZE;
        if (keystroke_times[idx] > limit) {
            active_clicks++;
        } else {
            break;
        }
    }
    k_spin_unlock(&speed_lock, key);

    return (int)((int64_t)active_clicks * 10000 / SPEED_WINDOW_MS);
}

/* ══════════════════════════════════════════════════════════════════ */
/*                       Modifier Status                           */
/* ══════════════════════════════════════════════════════════════════ */

static void apply_modifier_status(void *unused) {
    ARG_UNUSED(unused);

    if (modifier_status_row == NULL) {
        return;
    }

    uint8_t mods = pending_modifier_mask;
    bool any_active = false;

    // Caps Word
    bool caps_active = false;
#ifdef CONFIG_ZMK_CAPS_WORD
    if (caps_word_active) {
        caps_active = true;
    }
#endif
    if (caps_active) {
        lv_obj_clear_flag(mod_boxes[0], LV_OBJ_FLAG_HIDDEN);
        any_active = true;
    } else {
        lv_obj_add_flag(mod_boxes[0], LV_OBJ_FLAG_HIDDEN);
    }

    // Ctrl
    if (mods & (MOD_LCTL | MOD_RCTL)) {
        lv_obj_clear_flag(mod_boxes[1], LV_OBJ_FLAG_HIDDEN);
        any_active = true;
    } else {
        lv_obj_add_flag(mod_boxes[1], LV_OBJ_FLAG_HIDDEN);
    }

    // Shift
    if (mods & (MOD_LSFT | MOD_RSFT)) {
        lv_obj_clear_flag(mod_boxes[2], LV_OBJ_FLAG_HIDDEN);
        any_active = true;
    } else {
        lv_obj_add_flag(mod_boxes[2], LV_OBJ_FLAG_HIDDEN);
    }

    // Alt
    if (mods & (MOD_LALT | MOD_RALT)) {
        lv_obj_clear_flag(mod_boxes[3], LV_OBJ_FLAG_HIDDEN);
        any_active = true;
    } else {
        lv_obj_add_flag(mod_boxes[3], LV_OBJ_FLAG_HIDDEN);
    }

    // Win
    if (mods & (MOD_LGUI | MOD_RGUI)) {
        lv_obj_clear_flag(mod_boxes[4], LV_OBJ_FLAG_HIDDEN);
        any_active = true;
    } else {
        lv_obj_add_flag(mod_boxes[4], LV_OBJ_FLAG_HIDDEN);
    }

    if (any_active) {
        lv_obj_clear_flag(modifier_status_row, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(modifier_status_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_modifier_status_mask(uint8_t mask) {
    if (mask == pending_modifier_mask) {
        return;
    }

    pending_modifier_mask = mask;
    lv_async_call(apply_modifier_status, NULL);
}

static uint8_t modifier_mask_from_position(uint32_t position) {
    /* Physical modifier positions from the base keymap. */
    switch (position) {
    case 12:
    case 27:
        return MOD_LCTL;
    case 58:
        return MOD_RCTL;
    case 18:
    case 28:
        return MOD_LSFT;
    case 61:
        return MOD_RALT;
    case 29:
        return MOD_LGUI;
    case 60:
        return MOD_RGUI;
    default:
        return 0;
    }
}

static void update_modifier_status_from_position(uint32_t position, bool pressed) {
    uint8_t mods = modifier_mask_from_position(position);

    if (mods == 0) {
        return;
    }

    for (uint8_t i = 0; i < 8; i++) {
        if ((mods & (1U << i)) == 0) {
            continue;
        }

        if (pressed) {
            if (active_modifier_counts[i] < UINT8_MAX) {
                active_modifier_counts[i]++;
            }
        } else if (active_modifier_counts[i] > 0) {
            active_modifier_counts[i]--;
        }
    }

    uint8_t mask = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (active_modifier_counts[i] > 0) {
            mask |= 1U << i;
        }
    }

    position_modifier_mask = mask;
    set_modifier_status_mask(position_modifier_mask |
                             zmk_hid_get_keyboard_report()->body.modifiers);
}

static void modifier_status_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!display_screen_ready) {
        return;
    }

    uint8_t mods = position_modifier_mask |
                   zmk_hid_get_keyboard_report()->body.modifiers;
    set_modifier_status_mask(mods);

    if (wps_label != NULL) {
        int kps_x10 = calc_kps_x10();
        // WPS (Words Per Second) * 10 = kps_x10 / 5
        int wps_val = kps_x10 / 5;
        lv_label_set_text_fmt(wps_label, "WPS:%03d", wps_val);
    }

    k_work_reschedule(&modifier_status_work, K_MSEC(MOD_STATUS_TICK_MS));
}

/* ══════════════════════════════════════════════════════════════════ */
/*                       Layer Status                              */
/* ══════════════════════════════════════════════════════════════════ */

static void active_label_slide_anim_cb(void *var, int32_t val) {
    lv_obj_t *badge = (lv_obj_t *)var;
    int32_t y_pos = (badge == fn_layer_badge) ? 31 : 8;
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -10 + val, y_pos);
}

static void trigger_slide_in(lv_obj_t *badge) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, badge);
    lv_anim_set_values(&a, 60, 0);                 // Slide in from +60px right
    lv_anim_set_time(&a, 250);                     // 250ms duration (springy overshoot)
    lv_anim_set_exec_cb(&a, active_label_slide_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot); // Use LVGL's built-in spring overshoot curve!
    lv_anim_start(&a);
}

static void apply_layer_status(void *unused) {
    ARG_UNUSED(unused);

    if (base_layer_badge == NULL || base_layer_label == NULL ||
        fn_layer_badge == NULL || fn_layer_label == NULL) {
        return;
    }

    uint8_t layer = zmk_keymap_highest_layer_active();

    if (layer == LAYER_FN_INDEX) {
        // BASE layer inactive: transparent background, gray text
        lv_obj_set_style_bg_opa(base_layer_badge, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(base_layer_label, lv_color_hex(0x606060), LV_PART_MAIN);
        lv_label_set_text(base_layer_label, "  BASE");

        // FN layer active: red background, white text
        lv_obj_set_style_bg_color(fn_layer_badge, lv_color_hex(0xC63939), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(fn_layer_badge, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(fn_layer_label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(fn_layer_label, "> FN ");

        trigger_slide_in(fn_layer_badge);
    } else {
        // BASE layer active: blue background, white text
        lv_obj_set_style_bg_color(base_layer_badge, lv_color_hex(0x2B5C8F), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(base_layer_badge, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(base_layer_label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(base_layer_label, "> BASE ");

        // FN layer inactive: transparent background, gray text
        lv_obj_set_style_bg_opa(fn_layer_badge, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(fn_layer_label, lv_color_hex(0x606060), LV_PART_MAIN);
        lv_label_set_text(fn_layer_label, "  FN");

        trigger_slide_in(base_layer_badge);
    }
}

/* ══════════════════════════════════════════════════════════════════ */
/*                       Battery Status                            */
/* ══════════════════════════════════════════════════════════════════ */

static void clear_obj_style(lv_obj_t *obj) {
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

static void apply_battery_status(void *unused) {
    ARG_UNUSED(unused);

    if (battery_status_row == NULL) {
        return;
    }

    uint8_t levels[BATTERY_SLOT_COUNT];
    bool known[BATTERY_SLOT_COUNT];
    bool connected[BATTERY_SLOT_COUNT];

    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    for (size_t i = 0; i < BATTERY_SLOT_COUNT; i++) {
        levels[i] = battery_levels[i];
        known[i] = battery_level_known[i];
        connected[i] = battery_connected[i];
    }
    k_spin_unlock(&battery_lock, key);

    for (size_t i = 0; i < BATTERY_SLOT_COUNT; i++) {
        struct battery_slot_obj *slot = &battery_slots[i];

        if (connected[i]) {
            // Restore connected colors (white battery borders and white text)
            lv_obj_set_style_border_color(slot->icon_body, lv_color_white(), LV_PART_MAIN);
            lv_obj_set_style_bg_color(slot->icon_cap, lv_color_white(), LV_PART_MAIN);
            lv_obj_set_style_text_color(slot->num, lv_color_white(), LV_PART_MAIN);

            if (known[i]) {
                lv_bar_set_value(slot->bar, levels[i], LV_ANIM_ON);
                lv_label_set_text_fmt(slot->num, "%d", levels[i]);

                // Level-based status colors
                if (levels[i] > 50) {
                    lv_obj_set_style_bg_color(slot->bar, lv_color_hex(0x4CAF50), LV_PART_INDICATOR);
                } else if (levels[i] >= 20) {
                    lv_obj_set_style_bg_color(slot->bar, lv_color_hex(0xF57C00), LV_PART_INDICATOR);
                } else {
                    lv_obj_set_style_bg_color(slot->bar, lv_color_hex(0xD32F2F), LV_PART_INDICATOR);
                }
            } else {
                lv_bar_set_value(slot->bar, 0, LV_ANIM_ON);
                lv_label_set_text(slot->num, "--");
            }
        } else {
            // Disconnected: Grey outline, grey text, empty bar
            lv_obj_set_style_border_color(slot->icon_body, lv_color_hex(0x606060), LV_PART_MAIN);
            lv_obj_set_style_bg_color(slot->icon_cap, lv_color_hex(0x606060), LV_PART_MAIN);
            lv_obj_set_style_text_color(slot->num, lv_color_hex(0x606060), LV_PART_MAIN);

            lv_bar_set_value(slot->bar, 0, LV_ANIM_OFF);
            lv_label_set_text(slot->num, "--");
        }
    }
}

static void set_battery_level(uint8_t source, uint8_t level) {
    if (source >= BATTERY_SLOT_COUNT) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&battery_lock);
    battery_levels[source] = level;
    battery_level_known[source] = level > 0;
    battery_connected[source] = level > 0;
    k_spin_unlock(&battery_lock, key);

    if (display_screen_ready) {
        lv_async_call(apply_battery_status, NULL);
    }
}

static void create_battery_status(lv_obj_t *screen) {
    battery_status_row = lv_obj_create(screen);
    lv_obj_set_size(battery_status_row, lv_pct(100), BATTERY_STATUS_H);
    lv_obj_align(battery_status_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    clear_obj_style(battery_status_row);

    // Left battery slots (16px font size, visual height 12px, aligned at bottom y = 123)
    // Relative position in 48px row: y_pos_16 = 111 - 87 = 24
    int y_pos_16 = 24;

    for (size_t i = 0; i < BATTERY_SLOT_COUNT; i++) {
        lv_obj_t *slot = lv_obj_create(battery_status_row);
        clear_obj_style(slot);
        lv_obj_set_size(slot, LV_SIZE_CONTENT, 12);
        lv_obj_set_pos(slot, 18 + i * 60, y_pos_16);

        // Align child widgets horizontally inside the slot
        lv_obj_set_layout(slot, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(slot, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(slot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(slot, 6, LV_PART_MAIN);

        // Custom pixel-art battery icon container (width 26, height 12)
        lv_obj_t *icon_box = lv_obj_create(slot);
        clear_obj_style(icon_box);
        lv_obj_set_size(icon_box, 26, 12);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        // Battery outer outline box (width 24, height 12, radius 0, transparent bg, 1px border)
        battery_slots[i].icon_body = lv_obj_create(icon_box);
        lv_obj_set_size(battery_slots[i].icon_body, 24, 12);
        lv_obj_align(battery_slots[i].icon_body, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_opa(battery_slots[i].icon_body, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(battery_slots[i].icon_body, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(battery_slots[i].icon_body, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_radius(battery_slots[i].icon_body, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(battery_slots[i].icon_body, 0, LV_PART_MAIN);
        lv_obj_clear_flag(battery_slots[i].icon_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        // Battery terminal cap (width 2, height 6, aligned to the right center)
        battery_slots[i].icon_cap = lv_obj_create(icon_box);
        lv_obj_set_size(battery_slots[i].icon_cap, 2, 6);
        lv_obj_align(battery_slots[i].icon_cap, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(battery_slots[i].icon_cap, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(battery_slots[i].icon_cap, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(battery_slots[i].icon_cap, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(battery_slots[i].icon_cap, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(battery_slots[i].icon_cap, 0, LV_PART_MAIN);
        lv_obj_clear_flag(battery_slots[i].icon_cap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        // Battery fill indicator (width 22, height 10 inside the 24x12 outer box)
        battery_slots[i].bar = lv_bar_create(battery_slots[i].icon_body);
        lv_obj_set_size(battery_slots[i].bar, 22, 10);
        lv_obj_align(battery_slots[i].bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_opa(battery_slots[i].bar, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(battery_slots[i].bar, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(battery_slots[i].bar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(battery_slots[i].bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_anim_time(battery_slots[i].bar, 250, 0);
        lv_bar_set_range(battery_slots[i].bar, 0, 100);
        lv_bar_set_value(battery_slots[i].bar, 0, LV_ANIM_OFF);

        // Percentage text label (silkscreen_regular_16)
        battery_slots[i].num = lv_label_create(slot);
        lv_obj_set_style_text_font(battery_slots[i].num, &silkscreen_regular_16,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_color(battery_slots[i].num, lv_color_white(),
                                    LV_PART_MAIN);
        lv_obj_set_style_translate_y(battery_slots[i].num, -2, LV_PART_MAIN);
        lv_label_set_text(battery_slots[i].num, "--");
    }

    // Right WPS speed label (16px font size, visual height 10px, aligned at bottom y = 123)
    wps_label = lv_label_create(battery_status_row);
    lv_obj_set_style_text_font(wps_label, &silkscreen_regular_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(wps_label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(wps_label, LV_ALIGN_TOP_RIGHT, -18, 20); // 顶端偏移 20px 配合 16px 字体达到底端 aligned
    lv_label_set_text(wps_label, "WPS:000");

    apply_battery_status(NULL);
}

/* ══════════════════════════════════════════════════════════════════ */
/*                       Display Screen Setup                       */
/* ══════════════════════════════════════════════════════════════════ */

static void create_modifier_status(lv_obj_t *screen) {
    modifier_status_row = lv_obj_create(screen);
    lv_obj_set_size(modifier_status_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(modifier_status_row, LV_ALIGN_BOTTOM_MID, 0, MOD_STATUS_BOTTOM_Y);
    lv_obj_set_style_bg_opa(modifier_status_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(modifier_status_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(modifier_status_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(modifier_status_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_layout(modifier_status_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(modifier_status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(modifier_status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(modifier_status_row, 4, LV_PART_MAIN);

    const lv_img_dsc_t *symbols[] = {
        &caps_symbol_img,
        &ctrl_symbol_img,
        &shift_symbol_img,
        &alt_symbol_img,
        &win_symbol_img
    };

    for (size_t i = 0; i < 5; i++) {
        // Parent box: solid white background
        mod_boxes[i] = lv_obj_create(modifier_status_row);
        lv_obj_set_size(mod_boxes[i], 23, 23);
        lv_obj_set_style_bg_color(mod_boxes[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(mod_boxes[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(mod_boxes[i], 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(mod_boxes[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(mod_boxes[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(mod_boxes[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        // Inner box: black background with 1px white border, shifted top-left
        lv_obj_t *inner_box = lv_obj_create(mod_boxes[i]);
        lv_obj_set_size(inner_box, 21, 21);
        lv_obj_align(inner_box, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(inner_box, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(inner_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(inner_box, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(inner_box, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(inner_box, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(inner_box, 0, LV_PART_MAIN);
        lv_obj_clear_flag(inner_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        // Icon image inside inner box
        lv_obj_t *img = lv_img_create(inner_box);
        lv_img_set_src(img, symbols[i]);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_img_recolor(img, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);

        // Initially hidden
        lv_obj_add_flag(mod_boxes[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(modifier_status_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modifier_status_row);
}

static void create_layer_status(lv_obj_t *screen) {
    // Create BASE layer badge container
    base_layer_badge = lv_obj_create(screen);
    clear_obj_style(base_layer_badge);
    lv_obj_set_size(base_layer_badge, LV_SIZE_CONTENT, 20);
    lv_obj_set_style_radius(base_layer_badge, 3, LV_PART_MAIN);
    lv_obj_set_layout(base_layer_badge, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(base_layer_badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(base_layer_badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(base_layer_badge, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(base_layer_badge, 6, LV_PART_MAIN);
    lv_obj_align(base_layer_badge, LV_ALIGN_TOP_RIGHT, -10, 8);

    base_layer_label = lv_label_create(base_layer_badge);
    lv_obj_set_style_text_font(base_layer_label, &silkscreen_bold_16, LV_PART_MAIN);
    lv_obj_set_style_translate_y(base_layer_label, -1, LV_PART_MAIN); // 垂直微调 1 像素以完美对齐

    // Create FN layer badge container
    fn_layer_badge = lv_obj_create(screen);
    clear_obj_style(fn_layer_badge);
    lv_obj_set_size(fn_layer_badge, LV_SIZE_CONTENT, 20);
    lv_obj_set_style_radius(fn_layer_badge, 3, LV_PART_MAIN);
    lv_obj_set_layout(fn_layer_badge, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(fn_layer_badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fn_layer_badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(fn_layer_badge, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(fn_layer_badge, 6, LV_PART_MAIN);
    lv_obj_align(fn_layer_badge, LV_ALIGN_TOP_RIGHT, -10, 31); // 8 + 20 + 3 = 31

    fn_layer_label = lv_label_create(fn_layer_badge);
    lv_obj_set_style_text_font(fn_layer_label, &silkscreen_bold_16, LV_PART_MAIN);
    lv_obj_set_style_translate_y(fn_layer_label, -1, LV_PART_MAIN); // 垂直微调 1 像素以完美对齐

    apply_layer_status(NULL);
    lv_obj_move_foreground(base_layer_badge);
    lv_obj_move_foreground(fn_layer_badge);
}

static void create_bongo_cat(lv_obj_t *screen) {
    cat_container = lv_obj_create(screen);
    lv_obj_set_size(cat_container, CAT_CONTAINER_W, CAT_CONTAINER_H);
    lv_obj_align(cat_container, LV_ALIGN_CENTER, CAT_CONTAINER_X, CAT_CONTAINER_Y);
    lv_obj_set_style_bg_opa(cat_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cat_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cat_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cat_container, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    bongo_cat_img = lv_img_create(cat_container);
    lv_img_set_src(bongo_cat_img, &bongo_resting);
    lv_obj_align(bongo_cat_img, LV_ALIGN_CENTER, CAT_X_OFFSET, CAT_Y_OFFSET);

    left_tap_mask = lv_obj_create(cat_container);
    right_tap_mask = lv_obj_create(cat_container);
    lv_obj_t *masks[] = {left_tap_mask, right_tap_mask};
    for (size_t i = 0; i < 2; i++) {
        lv_obj_clear_flag(masks[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(masks[i], lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(masks[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(masks[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(masks[i], 0, LV_PART_MAIN);
        lv_obj_add_flag(masks[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_move_foreground(cat_container);
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    create_battery_status(screen);
    create_bongo_cat(screen);
    create_layer_status(screen);
    create_modifier_status(screen);

    display_screen_ready = true;

    if (display_work_ready) {
        k_work_reschedule(&modifier_status_work, K_NO_WAIT);
    }

    return screen;
}

static int display_screen_init(void) {
    k_work_init_delayable(&bongo_frame_work, bongo_frame_work_handler);
    k_work_init_delayable(&bongo_down_work, bongo_down_work_handler);
    k_work_init_delayable(&bongo_return_work, bongo_return_work_handler);
    k_work_init_delayable(&bongo_busy_work, bongo_busy_work_handler);
    k_work_init_delayable(&modifier_status_work, modifier_status_work_handler);
    display_work_ready = true;

    if (display_screen_ready) {
        k_work_reschedule(&modifier_status_work, K_NO_WAIT);
    }

    return 0;
}

SYS_INIT(display_screen_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ══════════════════════════════════════════════════════════════════ */
/*                       ZMK Event Listener                         */
/* ══════════════════════════════════════════════════════════════════ */

static int bongo_cat_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *battery_ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (battery_ev != NULL) {
        set_battery_level(battery_ev->source, battery_ev->state_of_charge);
        return ZMK_EV_EVENT_BUBBLE;
    }

#ifdef CONFIG_ZMK_CAPS_WORD
    const struct zmk_caps_word_state_changed *caps_ev = as_zmk_caps_word_state_changed(eh);
    if (caps_ev != NULL) {
        caps_word_active = caps_ev->active;
        lv_async_call(apply_modifier_status, NULL);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    if (layer_ev != NULL) {
        lv_async_call(apply_layer_status, NULL);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        if (active_key_count < UINT8_MAX) {
            active_key_count++;
        }

        update_modifier_status_from_position(ev->position, true);

        /* Record timestamp for typing speed calculation */
        record_keystroke_time();

        if (calc_kps_x10() >= BONGO_BUSY_KPS_X10) {
            start_busy_animation();
        } else {
            stop_busy_animation();
            trigger_typing_frame(ev->position < BONGO_RIGHT_FIRST_POSITION);
        }
    } else {
        update_modifier_status_from_position(ev->position, false);

        if (active_key_count > 0) {
            active_key_count--;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dactyl_bongo_cat, bongo_cat_listener);
#ifdef CONFIG_ZMK_CAPS_WORD
ZMK_SUBSCRIPTION(dactyl_bongo_cat, zmk_caps_word_state_changed);
#endif
ZMK_SUBSCRIPTION(dactyl_bongo_cat, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(dactyl_bongo_cat, zmk_position_state_changed);
ZMK_SUBSCRIPTION(dactyl_bongo_cat, zmk_peripheral_battery_state_changed);
