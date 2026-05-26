#include <lvgl.h>
#include <stdint.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "bongo_cat_art.h"

/* ──────────────────────── Bongo Cat Settings ──────────────────────── */

#define BONGO_REST_DELAY_MS 160

enum bongo_cat_frame {
    BONGO_CAT_RESTING,
    BONGO_CAT_LEFT,
    BONGO_CAT_RIGHT,
};

/* ──────────────────────── Typing Speed Tracker ──────────────────────── */

#define SPEED_RING_SIZE     16    /* Track last 16 keystrokes             */
#define FLAME_DECAY_MS      2000  /* Flame dies 2 s after last keystroke  */

/* ──────────────────────── Flame Particle System ──────────────────────── */

#define MAX_FLAME_PARTICLES 18
#define FLAME_TICK_MS       80    /* ~12 FPS flame animation              */
#define FLAME_FP_SHIFT      4     /* 1/16 px fixed-point particle motion  */
#define FLAME_FP_ONE        (1 << FLAME_FP_SHIFT)

/*
 * Cat head position within the 204x120 cat container.
 * The bongo cat image is 152x78, centered in the container.
 * Image origin = ((204-152)/2, (120-78)/2) = (26, 21).
 * Head center is roughly at (26+90, 21+12) = (116, 33).
 */
#define FLAME_BASE_X        116   /* Head center X in container coords    */
#define FLAME_BASE_Y        30    /* Head top Y in container coords       */
#define FLAME_SPREAD_X      14    /* Horizontal spawn spread              */

typedef enum {
    FLAME_NONE,      /* < 0.8 KPS */
    FLAME_SMALL,     /* 0.8-2.8 KPS */
    FLAME_MEDIUM,    /* 2.8-5 KPS */
    FLAME_LARGE,     /* >= 5 KPS  */
} flame_level_t;

typedef struct {
    lv_obj_t *obj;
    int16_t   x_fp, y_fp;
    int16_t   life;
    int16_t   max_life;
    int16_t   vx_fp, vy_fp;
    int8_t    width;
    int8_t    height;
    uint8_t   heat;
} flame_particle_t;

/* ──────────────────────── Static Variables ──────────────────────── */

/* Bongo cat */
static struct k_work_delayable bongo_frame_work;
static struct k_work_delayable display_overlay_work;
static lv_obj_t *bongo_cat_img;
static lv_obj_t *cat_container;
static bool display_overlay_installed;
static enum bongo_cat_frame pending_bongo_frame = BONGO_CAT_RESTING;
static uint8_t active_key_count;
static bool use_left_frame = true;

/* Typing speed ring buffer (accessed from both event and LVGL contexts) */
static int64_t keystroke_times[SPEED_RING_SIZE];
static uint8_t speed_ring_head;
static uint8_t speed_ring_count;
static struct k_spinlock speed_lock;

/* Flame particles */
static flame_particle_t flame_particles[MAX_FLAME_PARTICLES];
static struct k_work_delayable flame_tick_work;
static bool flame_tick_running;
static uint32_t flame_rng_state = 0xDEADBEEF;

/* ══════════════════════════════════════════════════════════════════ */
/*                       Bongo Cat Animation                        */
/* ══════════════════════════════════════════════════════════════════ */

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
/*             Flame Particle System  (Balatro-style)               */
/* ══════════════════════════════════════════════════════════════════ */

/* Simple xorshift32 PRNG */
static uint32_t flame_rand(void) {
    flame_rng_state ^= flame_rng_state << 13;
    flame_rng_state ^= flame_rng_state >> 17;
    flame_rng_state ^= flame_rng_state << 5;
    return flame_rng_state;
}

static int16_t clamp_i16(int16_t value, int16_t min, int16_t max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static int snap_px(int value) {
    return value & ~1;
}

static void spawn_particle(flame_particle_t *p, flame_level_t level) {
    uint32_t r = flame_rand();
    int16_t spread = FLAME_SPREAD_X;
    int16_t level_boost = 0;

    switch (level) {
    case FLAME_LARGE:
        spread += 4;
        level_boost = 5;
        break;
    case FLAME_MEDIUM:
        spread += 2;
        level_boost = 3;
        break;
    case FLAME_SMALL:
        level_boost = 1;
        break;
    default:
        break;
    }

    int16_t spawn_x = FLAME_BASE_X
                      + (int16_t)(r % (spread * 2 + 1)) - spread;
    int16_t spawn_y = FLAME_BASE_Y + 4 + (int16_t)(flame_rand() % 6);
    int16_t center_dist = spawn_x > FLAME_BASE_X ? spawn_x - FLAME_BASE_X
                                                 : FLAME_BASE_X - spawn_x;

    p->x_fp     = spawn_x * FLAME_FP_ONE;
    p->y_fp     = spawn_y * FLAME_FP_ONE;
    p->life     = 7 + level_boost + (int16_t)(flame_rand() % 7);
    p->max_life = p->life;
    p->vx_fp    = (int16_t)(flame_rand() % 21) - 10;     /* -0.63..0.63px */
    p->vy_fp    = FLAME_FP_ONE + 6 + level_boost + (int16_t)(flame_rand() % 21);
    p->width    = 4 + (int8_t)((flame_rand() % 3) * 2);  /* 4, 6, 8 px    */
    p->height   = 4 + level_boost + (int8_t)((flame_rand() % 4) * 2);

    if (center_dist <= spread / 3 && (flame_rand() % 3) == 0) {
        p->heat = 2; /* yellow core */
    } else if ((flame_rand() % 4) != 0) {
        p->heat = 1; /* orange body */
    } else {
        p->heat = 0; /* red edge */
    }

    /* Lazy-create the LVGL object on first use */
    if (p->obj == NULL) {
        p->obj = lv_obj_create(cat_container);
        lv_obj_clear_flag(p->obj,
                          LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(p->obj, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(p->obj, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(p->obj, 0, LV_PART_MAIN);
    }

    /* Set initial appearance immediately to avoid a flash of default style */
    lv_obj_set_style_bg_color(p->obj, lv_color_make(244, 58, 0), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p->obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_size(p->obj, p->width, p->height);
    lv_obj_clear_flag(p->obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(p->obj, snap_px(spawn_x - p->width / 2),
                   snap_px(spawn_y - p->height / 2));
}

static void update_particle(flame_particle_t *p) {
    if (p->life <= 0 || p->obj == NULL) {
        if (p->obj != NULL) {
            lv_obj_add_flag(p->obj, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Physics: rise upward with sub-pixel drift and small turbulence. */
    p->y_fp -= p->vy_fp;
    p->x_fp += p->vx_fp;
    if (flame_rand() % 3 == 0) {
        p->vx_fp += (int16_t)(flame_rand() % 7) - 3;
        p->vx_fp = clamp_i16(p->vx_fp, -14, 14);
    }
    if (flame_rand() % 5 == 0) {
        p->vy_fp += (int16_t)(flame_rand() % 5) - 2;
        p->vy_fp = clamp_i16(p->vy_fp, FLAME_FP_ONE - 2, FLAME_FP_ONE * 3);
    }
    p->life--;

    /* Blocky Balatro-style palette: red/orange body, yellow only in the core. */
    int ratio = (p->life * 100) / p->max_life;
    lv_color_t color;

    if (ratio > 72) {
        if (p->heat >= 2) {
            color = lv_color_make(255, 232, 68);
        } else if (p->heat == 1) {
            color = lv_color_make(255, 112, 0);
        } else {
            color = lv_color_make(224, 36, 0);
        }
    } else if (ratio > 38) {
        if (p->heat >= 2) {
            color = lv_color_make(255, 132, 0);
        } else if (p->heat == 1) {
            color = lv_color_make(236, 58, 0);
        } else {
            color = lv_color_make(156, 16, 6);
        }
    } else {
        if (p->heat >= 2) {
            color = lv_color_make(190, 44, 0);
        } else if (p->heat == 1) {
            color = lv_color_make(130, 14, 12);
        } else {
            color = lv_color_make(72, 0, 18);
        }
    }

    lv_obj_set_style_bg_color(p->obj, color, LV_PART_MAIN);

    /* Opacity fades with life */
    lv_opa_t opa = (lv_opa_t)(LV_OPA_40 +
                              ratio * (LV_OPA_COVER - LV_OPA_40) / 100);
    lv_obj_set_style_bg_opa(p->obj, opa, LV_PART_MAIN);

    /* Keep the blocks chunky, then snap them to a 2 px grid. */
    int new_width = 2 + (p->width - 2) * ratio / 100;
    int new_height = 2 + (p->height - 2) * ratio / 100;
    if (new_width < 2) {
        new_width = 2;
    }
    if (new_height < 2) {
        new_height = 2;
    }
    new_width = snap_px(new_width);
    new_height = snap_px(new_height);

    lv_obj_set_size(p->obj, new_width, new_height);
    lv_obj_set_pos(p->obj, snap_px((p->x_fp / FLAME_FP_ONE) - new_width / 2),
                   snap_px((p->y_fp / FLAME_FP_ONE) - new_height / 2));
}

static void flame_tick_callback(void *unused) {
    ARG_UNUSED(unused);

    if (cat_container == NULL) {
        return;
    }

    flame_level_t level = get_flame_level();

    /* Target particle count per flame level */
    int target;
    switch (level) {
    case FLAME_LARGE:  target = 18; break;
    case FLAME_MEDIUM: target = 11; break;
    case FLAME_SMALL:  target = 5;  break;
    default:           target = 0;  break;
    }

    /* Update existing particles */
    int alive = 0;
    for (int i = 0; i < MAX_FLAME_PARTICLES; i++) {
        if (flame_particles[i].life > 0) {
            update_particle(&flame_particles[i]);
            if (flame_particles[i].life > 0) {
                alive++;
            }
        }
    }

    /* Spawn new particles; faster typing ramps the flame up harder. */
    int to_spawn = target - alive;
    int max_spawn;
    switch (level) {
    case FLAME_LARGE:  max_spawn = 4; break;
    case FLAME_MEDIUM: max_spawn = 3; break;
    case FLAME_SMALL:  max_spawn = 2; break;
    default:           max_spawn = 0; break;
    }
    if (to_spawn > max_spawn) {
        to_spawn = max_spawn;
    }
    for (int i = 0; i < MAX_FLAME_PARTICLES && to_spawn > 0; i++) {
        if (flame_particles[i].life <= 0) {
            spawn_particle(&flame_particles[i], level);
            to_spawn--;
        }
    }

    /* Make sure dead particles stay hidden */
    for (int i = 0; i < MAX_FLAME_PARTICLES; i++) {
        if (flame_particles[i].life <= 0 && flame_particles[i].obj != NULL) {
            lv_obj_add_flag(flame_particles[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Self-stop: if no flame is needed and all particles are dead,
     * cancel the next scheduled tick.  The keystroke listener will
     * restart us when typing resumes. */
    if (level == FLAME_NONE && alive == 0 && to_spawn <= 0) {
        flame_tick_running = false;
        k_work_cancel_delayable(&flame_tick_work);
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

    lv_obj_t *layer_roller = lv_obj_get_child(screen, child_count - 1);
    lv_obj_set_size(layer_roller, 112, 64);
    lv_obj_align(layer_roller, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_text_font(layer_roller, LV_FONT_DEFAULT, LV_PART_MAIN);
    lv_obj_set_style_text_font(layer_roller, LV_FONT_DEFAULT, LV_PART_SELECTED);
}

static void move_caps_word_indicator(lv_obj_t *screen) {
    if (lv_obj_get_child_cnt(screen) < 1) {
        return;
    }

    lv_obj_t *caps_word_indicator = lv_obj_get_child(screen, 0);
    lv_obj_align(caps_word_indicator, LV_ALIGN_TOP_LEFT, 16, 88);
}

static void create_bongo_cat(lv_obj_t *screen) {
    cat_container = lv_obj_create(screen);
    lv_obj_set_size(cat_container, 204, 120);
    lv_obj_align(cat_container, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_bg_opa(cat_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cat_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cat_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cat_container, LV_OBJ_FLAG_SCROLLABLE);

    bongo_cat_img = lv_img_create(cat_container);
    lv_img_set_src(bongo_cat_img, &bongo_resting);
    lv_obj_center(bongo_cat_img);

    /* Flame particles are created lazily inside flame_tick_callback */
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

        schedule_bongo_frame(use_left_frame ? BONGO_CAT_LEFT : BONGO_CAT_RIGHT,
                             K_NO_WAIT);
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
