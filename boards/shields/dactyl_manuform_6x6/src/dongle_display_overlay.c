#include <lvgl.h>
#include <stdint.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "bongo_cat_art.h"

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
#define FLAME_DECAY_MS      2000  /* Flame dies 2 s after last keystroke  */

/* ──────────────────────── Flame Line Animation ──────────────────────── */

#define FLAME_TICK_MS       80    /* ~12 FPS flame animation              */
#define FLAME_STROKE_COUNT  7
#define FLAME_POINT_COUNT   4

/*
 * Cat and flame placement within the 204x128 cat container.
 * Cat frames use transparent indexed pixels so the foreground status
 * indicators can overlap without being covered by an image background.
 */
#define CAT_CONTAINER_W     204
#define CAT_CONTAINER_H     128
#define CAT_X_OFFSET        6
#define CAT_Y_OFFSET        10
#define CAT_CONTAINER_X     0
#define CAT_CONTAINER_Y     6
#define FLAME_BASE_X        120   /* Head center X in container coords    */
#define FLAME_BASE_Y        44    /* Flame base Y in container coords     */

typedef enum {
    FLAME_NONE,      /* < 0.8 KPS */
    FLAME_SMALL,     /* 0.8-2.8 KPS */
    FLAME_MEDIUM,    /* 2.8-5 KPS */
    FLAME_LARGE,     /* >= 5 KPS  */
} flame_level_t;

/* ──────────────────────── Static Variables ──────────────────────── */

/* Bongo cat */
static struct k_work_delayable bongo_frame_work;
static struct k_work_delayable bongo_return_work;
static struct k_work_delayable bongo_busy_work;
static struct k_work_delayable display_overlay_work;
static lv_obj_t *bongo_cat_img;
static lv_obj_t *cat_container;
static lv_obj_t *layer_roller_obj;
static lv_obj_t *caps_word_indicator_obj;
static bool display_overlay_installed;
static enum bongo_cat_frame pending_bongo_frame = BONGO_CAT_RESTING;
static uint8_t active_key_count;
static bool use_left_frame = true;
static bool busy_tick_running;
static uint8_t busy_phase;

/* Typing speed ring buffer (accessed from both event and LVGL contexts) */
static int64_t keystroke_times[SPEED_RING_SIZE];
static uint8_t speed_ring_head;
static uint8_t speed_ring_count;
static struct k_spinlock speed_lock;

typedef struct {
    lv_obj_t *obj;
    lv_point_t points[FLAME_POINT_COUNT];
} flame_stroke_t;

/* Flame strokes */
static struct k_work_delayable flame_tick_work;
static bool flame_tick_running;
static uint32_t flame_rng_state = 0xDEADBEEF;
static flame_stroke_t flame_strokes[FLAME_STROKE_COUNT];

/* ══════════════════════════════════════════════════════════════════ */
/*                       Bongo Cat Animation                        */
/* ══════════════════════════════════════════════════════════════════ */

static int calc_kps_x10(void);

static const lv_img_dsc_t *bongo_frame_image(enum bongo_cat_frame frame) {
    switch (frame) {
    case BONGO_CAT_LEFT:
        return &bongo_casualleft;
    case BONGO_CAT_RIGHT:
        return &bongo_casualright;
    case BONGO_CAT_BUSY:
        return &bongo_busy;
    case BONGO_CAT_BOTH:
        return &bongo_both;
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

    /* No recent activity -> no flame */
    if ((now - newest) > FLAME_DECAY_MS) {
        return 0;
    }

    int64_t span_ms = newest - oldest;
    if (span_ms <= 0) {
        return 0;
    }

    /* (count-1) keystrokes in span_ms milliseconds -> KPS x 10 */
    return (int)((int64_t)(count - 1) * 10000 / span_ms);
}

static flame_level_t get_flame_level(void) {
    int kps = calc_kps_x10();
    if (kps >= 50) return FLAME_LARGE;   /* >= 5.0 KPS (~60 WPM) */
    if (kps >= 28) return FLAME_MEDIUM;  /* >= 2.8 KPS (~34 WPM) */
    if (kps >= 8) return FLAME_SMALL;    /* >= 0.8 KPS (~10 WPM) */
    return FLAME_NONE;
}

/* ══════════════════════════════════════════════════════════════════ */
/*                  Flame Line Animation                            */
/* ══════════════════════════════════════════════════════════════════ */

/* Simple xorshift32 PRNG */
static uint32_t flame_rand(void) {
    flame_rng_state ^= flame_rng_state << 13;
    flame_rng_state ^= flame_rng_state >> 17;
    flame_rng_state ^= flame_rng_state << 5;
    return flame_rng_state;
}

static void hide_flame_strokes(void) {
    for (int i = 0; i < FLAME_STROKE_COUNT; i++) {
        if (flame_strokes[i].obj != NULL) {
            lv_obj_add_flag(flame_strokes[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static int flame_height_for_level(flame_level_t level) {
    switch (level) {
    case FLAME_LARGE:
        return 34;
    case FLAME_MEDIUM:
        return 26;
    case FLAME_SMALL:
        return 18;
    default:
        return 0;
    }
}

static int flame_active_strokes_for_level(flame_level_t level) {
    switch (level) {
    case FLAME_LARGE:
        return FLAME_STROKE_COUNT;
    case FLAME_MEDIUM:
        return 5;
    case FLAME_SMALL:
        return 3;
    default:
        return 0;
    }
}

static lv_color_t flame_color_for_index(int index) {
    switch (index) {
    case 0:
    case 4:
        return lv_color_make(190, 20, 0);
    case 1:
    case 3:
        return lv_color_make(235, 52, 0);
    case 2:
        return lv_color_make(255, 104, 0);
    case 5:
        return lv_color_make(255, 170, 20);
    case 6:
    default:
        return lv_color_make(255, 230, 70);
    }
}

static void set_flame_stroke(int index, int x_offset, int height, int width,
                             lv_color_t color, lv_opa_t opa) {
    flame_stroke_t *stroke = &flame_strokes[index];

    if (stroke->obj == NULL) {
        return;
    }

    int sway_a = (int)(flame_rand() % 9) - 4;
    int sway_b = (int)(flame_rand() % 13) - 6;
    int tip_jitter = (int)(flame_rand() % 7) - 3;
    int base_x = FLAME_BASE_X + x_offset;
    int base_y = FLAME_BASE_Y + (int)(flame_rand() % 3);

    stroke->points[0].x = base_x;
    stroke->points[0].y = base_y;
    stroke->points[1].x = base_x + sway_a;
    stroke->points[1].y = base_y - height / 3;
    stroke->points[2].x = base_x + sway_b;
    stroke->points[2].y = base_y - (height * 2) / 3;
    stroke->points[3].x = base_x + tip_jitter;
    stroke->points[3].y = base_y - height;

    lv_line_set_points(stroke->obj, stroke->points, FLAME_POINT_COUNT);
    lv_obj_set_style_line_color(stroke->obj, color, LV_PART_MAIN);
    lv_obj_set_style_line_width(stroke->obj, width, LV_PART_MAIN);
    lv_obj_set_style_line_opa(stroke->obj, opa, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(stroke->obj, true, LV_PART_MAIN);
    lv_obj_clear_flag(stroke->obj, LV_OBJ_FLAG_HIDDEN);
}

static void flame_tick_callback(void *unused) {
    ARG_UNUSED(unused);

    if (cat_container == NULL) {
        return;
    }

    flame_level_t level = get_flame_level();
    int active_strokes = flame_active_strokes_for_level(level);

    if (active_strokes == 0) {
        hide_flame_strokes();
        flame_tick_running = false;
        k_work_cancel_delayable(&flame_tick_work);
        return;
    }

    int height = flame_height_for_level(level);
    int width_boost = level == FLAME_LARGE ? 2 : level == FLAME_MEDIUM ? 1 : 0;

    static const int8_t offsets[FLAME_STROKE_COUNT] = {-16, -9, -3, 5, 12, 0, 2};
    static const uint8_t height_scale[FLAME_STROKE_COUNT] = {70, 86, 100, 78, 66, 58, 42};
    static const uint8_t width_base[FLAME_STROKE_COUNT] = {4, 5, 6, 5, 4, 3, 2};

    for (int i = 0; i < FLAME_STROKE_COUNT; i++) {
        if (i >= active_strokes) {
            lv_obj_add_flag(flame_strokes[i].obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        int stroke_height = (height * height_scale[i]) / 100 + (int)(flame_rand() % 5);
        int stroke_width = width_base[i] + width_boost;
        lv_opa_t opa = i >= 5 ? LV_OPA_90 : LV_OPA_COVER;
        set_flame_stroke(i, offsets[i], stroke_height, stroke_width,
                          flame_color_for_index(i), opa);
    }
}

static void flame_tick_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lv_async_call(flame_tick_callback, NULL);
    k_work_reschedule(&flame_tick_work, K_MSEC(FLAME_TICK_MS));
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

static void move_caps_word_indicator(lv_obj_t *screen) {
    if (lv_obj_get_child_cnt(screen) < 1) {
        return;
    }

    caps_word_indicator_obj = lv_obj_get_child(screen, 0);
    lv_obj_clear_flag(caps_word_indicator_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(caps_word_indicator_obj, LV_ALIGN_TOP_LEFT, 20, 82);
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

    for (int i = 0; i < FLAME_STROKE_COUNT; i++) {
        flame_strokes[i].obj = lv_line_create(cat_container);
        lv_obj_clear_flag(flame_strokes[i].obj,
                          LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(flame_strokes[i].obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_move_foreground(cat_container);
    if (layer_roller_obj != NULL) {
        lv_obj_move_foreground(layer_roller_obj);
    }
    if (caps_word_indicator_obj != NULL) {
        lv_obj_move_foreground(caps_word_indicator_obj);
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
    move_caps_word_indicator(screen);
    create_bongo_cat(screen);
    display_overlay_installed = true;

    /* Seed the flame RNG with hardware cycle counter */
    flame_rng_state = k_cycle_get_32() | 1u;

    /* Flame tick is NOT started here; it starts on first keystroke */
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
    k_work_init_delayable(&flame_tick_work, flame_tick_work_handler);
    k_work_schedule(&display_overlay_work, K_SECONDS(2));

    return 0;
}

SYS_INIT(display_overlay_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ══════════════════════════════════════════════════════════════════ */
/*                       ZMK Event Listener                         */
/* ══════════════════════════════════════════════════════════════════ */

static int bongo_cat_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state) {
        if (active_key_count < UINT8_MAX) {
            active_key_count++;
        }

        /* Record timestamp for typing speed / flame calculation */
        record_keystroke_time();

        /* Kick the flame animation if it's not already running */
        if (!flame_tick_running && display_overlay_installed) {
            flame_tick_running = true;
            k_work_reschedule(&flame_tick_work, K_NO_WAIT);
        }

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
ZMK_SUBSCRIPTION(dactyl_bongo_cat, zmk_position_state_changed);
