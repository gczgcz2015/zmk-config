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

/* ──────────────────────── Volcano Particle Animation ──────────────────────── */

#define FLAME_TICK_MS       80    /* ~12 FPS flame animation              */
#define FLAME_SPARK_COUNT   45

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

/* Flame Animation */
static struct k_work_delayable flame_tick_work;
static bool flame_tick_running;
static uint32_t flame_rng_state = 0xDEADBEEF;

typedef struct {
    int16_t x_q4;
    int16_t y_q4;
    int16_t vx_q4;
    int16_t vy_q4;
    int16_t origin_x;
    int16_t origin_y;
    int16_t life;
    int16_t max_life;
    uint8_t base_size;
    uint8_t color_index;
    bool right_side;
    bool rear_body;
} spark_state_t;

static lv_obj_t *flame_sparks[FLAME_SPARK_COUNT];
static spark_state_t spark_states[FLAME_SPARK_COUNT];

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

/* ══════════════════════════════════════════════════════════════════ */
/*                  Volcano Particle Animation                      */
/* ══════════════════════════════════════════════════════════════════ */

/* Simple xorshift32 PRNG */
static uint32_t flame_rand(void) {
    flame_rng_state ^= flame_rng_state << 13;
    flame_rng_state ^= flame_rng_state >> 17;
    flame_rng_state ^= flame_rng_state << 5;
    return flame_rng_state;
}

typedef struct {
    int16_t x;
    int16_t y;
} flame_emitter_t;

/*
 * V9.1 preview emitters converted from screen coordinates into this
 * cat_container. The conversion keeps them attached to the cat frame even
 * though the LVGL image is centered with CAT_X/Y_OFFSET.
 */
static const flame_emitter_t volcano_emitters[] = {
    {66, 74}, {76, 65}, {89, 58}, {101, 51}, {109, 47},
    {120, 56}, {134, 61}, {144, 67},
};

static const flame_emitter_t volcano_right_emitters[] = {
    {140, 65}, {148, 68}, {156, 71},
};

static const flame_emitter_t volcano_rear_emitters[] = {
    {152, 70}, {157, 75}, {159, 81},
};

static int rand_range(int min, int max) {
    return min + (int)(flame_rand() % (uint32_t)(max - min + 1));
}

static uint8_t mix_u8(uint8_t a, uint8_t b, uint8_t t) {
    return (uint8_t)(((uint16_t)a * (100 - t) + (uint16_t)b * t) / 100);
}

static const flame_emitter_t *pick_emitter(const flame_emitter_t *emitters,
                                           uint8_t count) {
    return &emitters[flame_rand() % count];
}

static void hide_flame_sparks(void) {
    for (int i = 0; i < FLAME_SPARK_COUNT; i++) {
        if (flame_sparks[i] != NULL) {
            lv_obj_add_flag(flame_sparks[i], LV_OBJ_FLAG_HIDDEN);
        }
        spark_states[i].life = 0;
    }
}

static uint8_t get_flame_heat(void) {
    int kps = calc_kps_x10();

    if (kps < 8) {
        return 0;
    }
    if (kps >= 60) {
        return 100;
    }

    int t = ((kps - 8) * 100) / (60 - 8);
    return (uint8_t)((t * t * (300 - 2 * t)) / 10000);
}

static lv_color_t volcano_spark_color(uint8_t heat, uint8_t color_index,
                                      int ratio) {
    static const uint8_t cold[3][3] = {
        {70, 170, 255},
        {175, 245, 255},
        {55, 80, 230},
    };
    static const uint8_t hot[3][3] = {
        {255, 95, 0},
        {255, 218, 48},
        {205, 28, 0},
    };

    uint8_t idx = color_index % 3;
    uint8_t r = mix_u8(cold[idx][0], hot[idx][0], heat);
    uint8_t g = mix_u8(cold[idx][1], hot[idx][1], heat);
    uint8_t b = mix_u8(cold[idx][2], hot[idx][2], heat);

    if (ratio < 35) {
        uint8_t ember_t = (uint8_t)(((35 - ratio) * heat) / 35);
        r = mix_u8(r, 80, ember_t);
        g = mix_u8(g, 36, ember_t);
        b = mix_u8(b, 18, ember_t);
    }

    return lv_color_make(r, g, b);
}

static void spawn_volcano_spark(spark_state_t *s, uint8_t heat) {
    uint8_t rear_threshold = 16 + (heat * 14) / 100;
    uint8_t right_threshold = 22 + (heat * 8) / 100;
    uint8_t roll = flame_rand() % 100;
    const flame_emitter_t *emitter;

    s->rear_body = false;
    s->right_side = false;

    if (roll < rear_threshold) {
        emitter = pick_emitter(volcano_rear_emitters,
                               ARRAY_SIZE(volcano_rear_emitters));
        s->rear_body = true;
        s->right_side = true;
    } else if (roll < right_threshold) {
        emitter = pick_emitter(volcano_right_emitters,
                               ARRAY_SIZE(volcano_right_emitters));
        s->right_side = true;
    } else {
        emitter = pick_emitter(volcano_emitters, ARRAY_SIZE(volcano_emitters));
        s->right_side = emitter->x >= 142;
    }

    s->origin_x = emitter->x;
    s->origin_y = emitter->y;
    s->x_q4 = (emitter->x + rand_range(-1, 1)) * 16;
    s->y_q4 = (emitter->y + rand_range(-1, 1)) * 16;
    s->max_life = s->rear_body ? rand_range(6, 11) : rand_range(8, 17);
    s->life = s->max_life;
    s->color_index = flame_rand() % 3;
    s->base_size = (heat > 55 && (flame_rand() % 100) < 42) ? 2 : 1;

    int side = rand_range(-45, 45);
    if ((flame_rand() % 100) < (uint32_t)((heat * 45) / 100)) {
        int extra = rand_range(15, 55);
        side += (flame_rand() & 1) ? extra : -extra;
    }
    if (s->right_side) {
        side = rand_range(-72, 8);
    }
    if (s->rear_body) {
        side = rand_range(-105, -34);
    }

    int spread = 7 + (heat * 22) / 100;
    int vx_total = (side * spread) / 100;
    int vy_min = 12 + (heat * 16) / 100;
    int vy_max = 22 + (heat * 30) / 100;
    int vy_total = -rand_range(vy_min, vy_max);

    if (heat > 55 && (flame_rand() % 100) < heat) {
        int burst = (rand_range(5, 14) * heat) / 100;
        vy_total -= burst;
        if (s->right_side) {
            vx_total -= (rand_range(2, 10) * heat) / 100;
        } else {
            int side_burst = (rand_range(2, 10) * heat) / 100;
            vx_total += (flame_rand() & 1) ? side_burst : -side_burst;
        }
    }

    s->vx_q4 = (vx_total * 16) / s->max_life;
    s->vy_q4 = (vy_total * 16) / s->max_life;
}

static bool volcano_spark_clipped(const spark_state_t *s, uint8_t heat) {
    int x = s->x_q4 / 16;
    int y = s->y_q4 / 16;
    int ratio = (s->life * 100) / s->max_life;
    int age = 100 - ratio;

    if (s->rear_body) {
        if (age > 50) {
            return true;
        }
        if (x > s->origin_x - 4 + heat / 100) {
            return true;
        }
        if (y < s->origin_y - (7 + (heat * 15) / 100)) {
            return true;
        }
        if (x > 164) {
            return true;
        }
    } else if (s->right_side && x > s->origin_x + 10 + (heat * 4) / 100) {
        return true;
    }

    if (s->right_side && x > 166) {
        return true;
    }
    if (heat < 45 && y < 41) {
        return true;
    }

    return y < 27 || y > 111 || x < 16 || x > 200;
}

static void render_volcano_spark(int index, const spark_state_t *s,
                                 uint8_t heat) {
    if (flame_sparks[index] == NULL) {
        return;
    }

    int ratio = (s->life * 100) / s->max_life;
    int size = s->base_size;
    if (size > 1 && ratio < 55) {
        size = 1;
    }

    uint16_t alpha_scale = (uint16_t)ratio * (60 + (heat * 40) / 100) / 100;
    lv_opa_t opa = (lv_opa_t)((uint16_t)LV_OPA_COVER * alpha_scale / 100);
    if (s->rear_body) {
        opa = (lv_opa_t)((uint16_t)opa * 82 / 100);
    }
    if (opa < 35) {
        lv_obj_add_flag(flame_sparks[index], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_size(flame_sparks[index], size, size);
    lv_obj_set_pos(flame_sparks[index], s->x_q4 / 16, s->y_q4 / 16);
    lv_obj_set_style_bg_color(flame_sparks[index],
                              volcano_spark_color(heat, s->color_index, ratio),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(flame_sparks[index], opa, LV_PART_MAIN);
    lv_obj_clear_flag(flame_sparks[index], LV_OBJ_FLAG_HIDDEN);
}

static void flame_tick_callback(void *unused) {
    ARG_UNUSED(unused);

    if (cat_container == NULL) {
        return;
    }

    uint8_t heat = get_flame_heat();

    if (heat == 0) {
        hide_flame_sparks();
        flame_tick_running = false;
        k_work_cancel_delayable(&flame_tick_work);
        return;
    }

    int max_sparks = 5 + (heat * (FLAME_SPARK_COUNT - 5)) / 100;
    int spawn_chance = 7 + (heat * 73) / 100;

    for (int i = 0; i < FLAME_SPARK_COUNT; i++) {
        if (i >= max_sparks) {
            if (flame_sparks[i] != NULL) {
                lv_obj_add_flag(flame_sparks[i], LV_OBJ_FLAG_HIDDEN);
            }
            spark_states[i].life = 0;
            continue;
        }

        spark_state_t *s = &spark_states[i];
        if (s->life <= 0) {
            if ((flame_rand() % 100) >= (uint32_t)spawn_chance) {
                if (flame_sparks[i] != NULL) {
                    lv_obj_add_flag(flame_sparks[i], LV_OBJ_FLAG_HIDDEN);
                }
                continue;
            }
            spawn_volcano_spark(s, heat);
        } else {
            s->x_q4 += s->vx_q4;
            s->y_q4 += s->vy_q4;
            s->vy_q4 += 4 + (heat * 3) / 100;
            s->life--;
            if (s->life > 0 && volcano_spark_clipped(s, heat)) {
                s->life = 0;
            }
        }

        if (s->life > 0) {
            render_volcano_spark(i, s, heat);
        } else {
            if (flame_sparks[i] != NULL) {
                lv_obj_add_flag(flame_sparks[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
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
    if (lv_obj_get_child_cnt(screen) < 3) {
        caps_word_indicator_obj = NULL;
        return;
    }

    caps_word_indicator_obj = lv_obj_get_child(screen, 0);
    lv_obj_clear_flag(caps_word_indicator_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(caps_word_indicator_obj, LV_ALIGN_RIGHT_MID, -10, 46);
}

static void create_bongo_cat(lv_obj_t *screen) {
    cat_container = lv_obj_create(screen);
    lv_obj_set_size(cat_container, CAT_CONTAINER_W, CAT_CONTAINER_H);
    lv_obj_align(cat_container, LV_ALIGN_CENTER, CAT_CONTAINER_X, CAT_CONTAINER_Y);
    lv_obj_set_style_bg_opa(cat_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cat_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cat_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cat_container, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 1. Pre-create sparks (behind the cat)
    for (int i = 0; i < FLAME_SPARK_COUNT; i++) {
        flame_sparks[i] = lv_obj_create(cat_container);
        lv_obj_clear_flag(flame_sparks[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(flame_sparks[i], 2, 2);
        lv_obj_set_style_radius(flame_sparks[i], 10, LV_PART_MAIN); // radius 10 handles dynamic size circular styling
        lv_obj_set_style_border_width(flame_sparks[i], 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(flame_sparks[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_flag(flame_sparks[i], LV_OBJ_FLAG_HIDDEN);
        spark_states[i].life = 0;
        spark_states[i].base_size = 2;
    }

    // 2. Create cat image on top of sparks (foreground)
    bongo_cat_img = lv_img_create(cat_container);
    lv_img_set_src(bongo_cat_img, &bongo_resting);
    lv_obj_align(bongo_cat_img, LV_ALIGN_CENTER, CAT_X_OFFSET, CAT_Y_OFFSET);

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
