#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>

#include "bongo_cat_art.h"

#ifdef CONFIG_ZMK_CAPS_WORD
#include <zmk/events/caps_word_state_changed.h>
#endif

/* ──────────────────────── Bongo Cat Settings ──────────────────────── */

#define BONGO_ACTIVE_MS     110
#define BONGO_BUSY_TICK_MS  120
#define BONGO_BUSY_KPS_X10  28

enum bongo_cat_frame {
    BONGO_CAT_RESTING,
    BONGO_CAT_LEFT,
    BONGO_CAT_RIGHT,
    BONGO_CAT_BUSY,
    BONGO_CAT_BOTH,
};

/* ──────────────────────── Typing Speed Tracker ──────────────────────── */

#define SPEED_RING_SIZE     16    /* Track last 16 keystrokes             */
#define BONGO_SPEED_DECAY_MS 2000 /* Speed drops to 0 after idle timeout  */

/*
 * The cat image is 204x120. Use a full-screen transparent container so
 * offsets cannot clip the larger fixed frame.
 */
#define CAT_CONTAINER_W     240
#define CAT_CONTAINER_H     135
#define CAT_X_OFFSET        0
#define CAT_Y_OFFSET        8
#define CAT_CONTAINER_X     0
#define CAT_CONTAINER_Y     0

/* ──────────────────────── Modifier Status ──────────────────────── */

#define MOD_STATUS_W        160

/* ──────────────────────── Static Variables ──────────────────────── */

/* Bongo cat */
static struct k_work_delayable bongo_frame_work;
static struct k_work_delayable bongo_return_work;
static struct k_work_delayable bongo_busy_work;
static struct k_work_delayable display_overlay_work;
static lv_obj_t *bongo_cat_img;
static lv_obj_t *cat_container;
static lv_obj_t *layer_roller_obj;
static lv_obj_t *modifier_status_label;
static bool display_overlay_installed;
static enum bongo_cat_frame pending_bongo_frame = BONGO_CAT_RESTING;
static uint8_t active_key_count;
static bool use_left_frame = true;
static bool busy_tick_running;
static uint8_t busy_phase;
static uint8_t pending_modifier_mask;
static uint8_t active_modifier_counts[8];
#ifdef CONFIG_ZMK_CAPS_WORD
static bool caps_word_active;
#endif

/* Typing speed ring buffer (accessed from both event and LVGL contexts) */
static int64_t keystroke_times[SPEED_RING_SIZE];
static uint8_t speed_ring_head;
static uint8_t speed_ring_count;
static struct k_spinlock speed_lock;

/* ══════════════════════════════════════════════════════════════════ */
/*                       Bongo Cat Animation                        */
/* ══════════════════════════════════════════════════════════════════ */

static int calc_kps_x10(void);

static const lv_img_dsc_t *bongo_frame_image(enum bongo_cat_frame frame) {
    switch (frame) {
    case BONGO_CAT_LEFT:
        /* The source names are from the original sprite sheet; visually this is the left paw. */
        return &bongo_casualright;
    case BONGO_CAT_RIGHT:
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
    case BONGO_CAT_LEFT:
        *x -= 4;
        *y += 1;
        break;
    case BONGO_CAT_RIGHT:
        *y -= 1;
        break;
    case BONGO_CAT_BUSY:
        *x -= 5;
        *y -= 2;
        break;
    case BONGO_CAT_BOTH:
    case BONGO_CAT_RESTING:
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
}

static void bongo_frame_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lv_async_call(apply_bongo_frame, NULL);
}

static void schedule_bongo_frame(enum bongo_cat_frame frame, k_timeout_t delay) {
    pending_bongo_frame = frame;
    k_work_reschedule(&bongo_frame_work, delay);
}

static void bongo_return_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!busy_tick_running) {
        schedule_bongo_frame(BONGO_CAT_RESTING, K_NO_WAIT);
    }
}

static void bongo_busy_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!display_overlay_installed || calc_kps_x10() < BONGO_BUSY_KPS_X10) {
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
    k_work_cancel_delayable(&bongo_return_work);

    if (busy_tick_running) {
        return;
    }

    busy_tick_running = true;
    busy_phase = 0;
    k_work_reschedule(&bongo_busy_work, K_NO_WAIT);
}

static void stop_busy_animation(void) {
    busy_tick_running = false;
    busy_phase = 0;
    k_work_cancel_delayable(&bongo_busy_work);
}

static void trigger_typing_frame(void) {
    k_work_cancel_delayable(&bongo_return_work);
    schedule_bongo_frame(use_left_frame ? BONGO_CAT_LEFT : BONGO_CAT_RIGHT,
                         K_NO_WAIT);
    use_left_frame = !use_left_frame;
    k_work_reschedule(&bongo_return_work, K_MSEC(BONGO_ACTIVE_MS));
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
    k_spinlock_key_t key = k_spin_lock(&speed_lock);
    uint8_t count = speed_ring_count;
    uint8_t head  = speed_ring_head;
    /* Snapshot the two timestamps we need while under lock */
    int64_t newest = 0, oldest = 0;
    if (count >= 2) {
        uint8_t newest_idx = (head - 1 + SPEED_RING_SIZE) % SPEED_RING_SIZE;
        uint8_t oldest_idx = (head - count + SPEED_RING_SIZE) % SPEED_RING_SIZE;
        newest = keystroke_times[newest_idx];
        oldest = keystroke_times[oldest_idx];
    }
    k_spin_unlock(&speed_lock, key);

    if (count < 2) {
        return 0;
    }

    int64_t now = k_uptime_get();

    /* No recent activity -> no busy animation */
    if ((now - newest) > BONGO_SPEED_DECAY_MS) {
        return 0;
    }

    int64_t span_ms = newest - oldest;
    if (span_ms <= 0) {
        return 0;
    }

    /* (count-1) keystrokes in span_ms milliseconds -> KPS x 10 */
    return (int)((int64_t)(count - 1) * 10000 / span_ms);
}

/* ══════════════════════════════════════════════════════════════════ */
/*                       Modifier Status                           */
/* ══════════════════════════════════════════════════════════════════ */

static void append_mod_text(char *text, size_t *idx, size_t len,
                            const char *label) {
    if (*idx > 0 && *idx + 1 < len) {
        text[*idx] = ' ';
        (*idx)++;
    }

    while (*label != '\0' && *idx + 1 < len) {
        text[*idx] = *label;
        (*idx)++;
        label++;
    }
}

static void apply_modifier_status(void *unused) {
    ARG_UNUSED(unused);

    if (modifier_status_label == NULL) {
        return;
    }

    uint8_t mods = pending_modifier_mask;
    char text[24];
    size_t idx = 0;

#ifdef CONFIG_ZMK_CAPS_WORD
    if (caps_word_active) {
        append_mod_text(text, &idx, sizeof(text), "CAPS");
    }
#endif
    if (mods & (MOD_LCTL | MOD_RCTL)) {
        append_mod_text(text, &idx, sizeof(text), "CTRL");
    }
    if (mods & (MOD_LSFT | MOD_RSFT)) {
        append_mod_text(text, &idx, sizeof(text), "SHIFT");
    }
    if (mods & (MOD_LALT | MOD_RALT)) {
        append_mod_text(text, &idx, sizeof(text), "ALT");
    }
    if (mods & (MOD_LGUI | MOD_RGUI)) {
        append_mod_text(text, &idx, sizeof(text), "GUI");
    }

    text[idx] = '\0';
    lv_label_set_text(modifier_status_label, text);

    if (idx == 0) {
        lv_obj_add_flag(modifier_status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(modifier_status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static uint8_t modifier_mask_from_keycode(const struct zmk_keycode_state_changed *ev) {
    uint8_t mods = ev->explicit_modifiers | ev->implicit_modifiers;

    if (is_mod(ev->usage_page, ev->keycode)) {
        mods |= 1U << (ev->keycode - HID_USAGE_KEY_KEYBOARD_LEFTCONTROL);
    }

    return mods;
}

static void update_modifier_status_from_keycode(const struct zmk_keycode_state_changed *ev) {
    uint8_t mods = modifier_mask_from_keycode(ev);

    if (mods == 0) {
        return;
    }

    for (uint8_t i = 0; i < 8; i++) {
        if ((mods & (1U << i)) == 0) {
            continue;
        }

        if (ev->state) {
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

    pending_modifier_mask = mask;
    lv_async_call(apply_modifier_status, NULL);
}

/* ══════════════════════════════════════════════════════════════════ */
/*                       Display Overlay Setup                      */
/* ══════════════════════════════════════════════════════════════════ */

static void shrink_layer_roller(lv_obj_t *screen) {
    uint32_t child_count = lv_obj_get_child_cnt(screen);

    if (child_count == 0) {
        return;
    }

    layer_roller_obj = lv_obj_get_child(screen, child_count - 1);
    lv_obj_set_size(layer_roller_obj, 104, 58);
    lv_obj_align(layer_roller_obj, LV_ALIGN_TOP_LEFT, 8, 18);
    lv_obj_set_style_text_font(layer_roller_obj, LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_font(layer_roller_obj, LV_FONT_DEFAULT, LV_PART_SELECTED);
}

static void hide_builtin_caps_word_indicator(lv_obj_t *screen) {
    if (lv_obj_get_child_cnt(screen) < 3) {
        return;
    }

    lv_obj_add_flag(lv_obj_get_child(screen, 0), LV_OBJ_FLAG_HIDDEN);
}

static void create_modifier_status(lv_obj_t *screen) {
    modifier_status_label = lv_label_create(screen);
    lv_obj_set_width(modifier_status_label, MOD_STATUS_W);
    lv_obj_set_style_text_font(modifier_status_label, LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_color(modifier_status_label, lv_color_white(),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(modifier_status_label, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_label_set_long_mode(modifier_status_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(modifier_status_label, "");
    lv_obj_align(modifier_status_label, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    lv_obj_add_flag(modifier_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modifier_status_label);
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

    lv_obj_move_foreground(cat_container);
    if (layer_roller_obj != NULL) {
        lv_obj_move_foreground(layer_roller_obj);
    }
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
    hide_builtin_caps_word_indicator(screen);
    create_bongo_cat(screen);
    create_modifier_status(screen);
    display_overlay_installed = true;
}

static void display_overlay_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lv_async_call(install_display_overlay, NULL);
}

static int display_overlay_init(void) {
    k_work_init_delayable(&bongo_frame_work, bongo_frame_work_handler);
    k_work_init_delayable(&bongo_return_work, bongo_return_work_handler);
    k_work_init_delayable(&bongo_busy_work, bongo_busy_work_handler);
    k_work_init_delayable(&display_overlay_work, display_overlay_work_handler);
    k_work_schedule(&display_overlay_work, K_SECONDS(2));

    return 0;
}

SYS_INIT(display_overlay_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ══════════════════════════════════════════════════════════════════ */
/*                       ZMK Event Listener                         */
/* ══════════════════════════════════════════════════════════════════ */

static int bongo_cat_listener(const zmk_event_t *eh) {
#ifdef CONFIG_ZMK_CAPS_WORD
    const struct zmk_caps_word_state_changed *caps_ev = as_zmk_caps_word_state_changed(eh);
    if (caps_ev != NULL) {
        caps_word_active = caps_ev->active;
        lv_async_call(apply_modifier_status, NULL);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    const struct zmk_keycode_state_changed *keycode_ev = as_zmk_keycode_state_changed(eh);
    if (keycode_ev != NULL) {
        update_modifier_status_from_keycode(keycode_ev);
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

        /* Record timestamp for typing speed calculation */
        record_keystroke_time();

        if (calc_kps_x10() >= BONGO_BUSY_KPS_X10) {
            start_busy_animation();
        } else {
            stop_busy_animation();
            trigger_typing_frame();
        }
    } else {
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
ZMK_SUBSCRIPTION(dactyl_bongo_cat, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(dactyl_bongo_cat, zmk_position_state_changed);
