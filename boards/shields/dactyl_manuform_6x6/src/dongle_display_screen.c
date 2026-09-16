#include <ctype.h>
#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <dt-bindings/zmk/modifiers.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_central_status_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>

LV_FONT_DECLARE(DINishExpanded_Light_36);
LV_FONT_DECLARE(DINish_Medium_24);
LV_FONT_DECLARE(FG_Medium_20);
LV_FONT_DECLARE(FR_Medium_32);

/* Operator layout palette from the reference Prospector screen. */
#define DISPLAY_COLOR_MOD_ACTIVE 0xb1e5f0
#define DISPLAY_COLOR_MOD_INACTIVE 0x3b527c

#define DISPLAY_COLOR_WPM_BAR_ACTIVE 0xc2526a
#define DISPLAY_COLOR_WPM_BAR_INACTIVE 0x242424
#define DISPLAY_COLOR_WPM_TEXT 0xc2526a

#define DISPLAY_COLOR_LAYER_TEXT 0xffffff
#define DISPLAY_COLOR_LAYER_DOT_ACTIVE 0xe0e0e0
#define DISPLAY_COLOR_LAYER_DOT_INACTIVE 0x575757

#define DISPLAY_COLOR_BATTERY_FILL 0x54806c
#define DISPLAY_COLOR_BATTERY_RING 0x2a4036
#define DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL 0x383c42
#define DISPLAY_COLOR_BATTERY_DISCONNECTED_RING 0x282c30
#define DISPLAY_COLOR_BATTERY_LOW_FILL 0xc08040
#define DISPLAY_COLOR_BATTERY_LOW_RING 0x584028

#define DISPLAY_COLOR_USB_ACTIVE_BG 0xb9b9a7
#define DISPLAY_COLOR_USB_INACTIVE_BG 0x4f4f40
#define DISPLAY_COLOR_BLE_ACTIVE_BG 0x569fa7
#define DISPLAY_COLOR_BLE_INACTIVE_BG 0x353f40
#define DISPLAY_COLOR_OUTPUT_ACTIVE_TEXT 0x000000
#define DISPLAY_COLOR_OUTPUT_INACTIVE_TEXT 0x7b7d93

#define DISPLAY_COLOR_SLOT_ACTIVE_BG 0x7b7d93
#define DISPLAY_COLOR_SLOT_INACTIVE_BG 0x353640

/* ───────────────────────────── Widget types ─────────────────────────── */

#define WPM_BAR_COUNT 26
#define WPM_MAX 120
#define PERIPHERAL_COUNT ZMK_SPLIT_BLE_PERIPHERAL_COUNT
#define LOW_BATTERY_THRESHOLD 20
#define LAYER_DOT_COUNT ZMK_KEYMAP_LAYERS_LEN

struct zmk_widget_wpm_meter {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *bars[WPM_BAR_COUNT];
    lv_obj_t *peak_indicator;
    lv_obj_t *wpm_label;
    lv_obj_t *layer_label;
};

struct layer_dots_widget {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *dots[LAYER_DOT_COUNT];
};

struct zmk_widget_modifier_indicator {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *mod_labels[4];
};

struct zmk_widget_battery_circles {
    sys_snode_t node;
    lv_obj_t *obj;
};

struct zmk_widget_output {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *usb_btn;
    lv_obj_t *ble_btn;
    lv_obj_t *slots[ZMK_BLE_PROFILE_COUNT];
};

static sys_slist_t wpm_widgets = SYS_SLIST_STATIC_INIT(&wpm_widgets);
static sys_slist_t layer_dots_widgets = SYS_SLIST_STATIC_INIT(&layer_dots_widgets);
static sys_slist_t modifier_widgets = SYS_SLIST_STATIC_INIT(&modifier_widgets);
static sys_slist_t output_widgets = SYS_SLIST_STATIC_INIT(&output_widgets);

/* ───────────────────────────── WPM meter ───────────────────────────── */

static struct k_work_delayable wpm_smooth_work;
static bool wpm_work_initialized;
static float displayed_wpm;
static float target_wpm;
static int previous_active_bars;
static int peak_position;
static int peak_hold_counter;
static int peak_decay_counter;

static const float smoothing_factor_up = 0.3f;
static const float smoothing_factor_down = 0.05f;

struct wpm_meter_state {
    uint8_t wpm;
};

static void wpm_meter_render(int active_bars) {
    struct zmk_widget_wpm_meter *widget;

    if (active_bars < 0) {
        active_bars = 0;
    } else if (active_bars > WPM_BAR_COUNT) {
        active_bars = WPM_BAR_COUNT;
    }

    SYS_SLIST_FOR_EACH_CONTAINER(&wpm_widgets, widget, node) {
        if (active_bars != previous_active_bars) {
            int min_bar = (active_bars < previous_active_bars) ? active_bars
                                                                : previous_active_bars;
            int max_bar = (active_bars > previous_active_bars) ? active_bars
                                                                : previous_active_bars;

            for (int i = min_bar; i < max_bar; i++) {
                lv_color_t color = (i < active_bars)
                                       ? lv_color_hex(DISPLAY_COLOR_WPM_BAR_ACTIVE)
                                       : lv_color_hex(DISPLAY_COLOR_WPM_BAR_INACTIVE);
                lv_obj_set_style_bg_color(widget->bars[i], color, LV_PART_MAIN);
            }
            previous_active_bars = active_bars;
        }

        if (peak_position > active_bars && peak_position > 0) {
            int bar_width = 8;
            int bar_gap = 2;
            int total_width = WPM_BAR_COUNT * bar_width + (WPM_BAR_COUNT - 1) * bar_gap;
            int start_x = (260 - total_width) / 2;
            int peak_slot = (peak_position > active_bars + 1) ? (peak_position - 1)
                                                               : active_bars;
            if (peak_slot >= WPM_BAR_COUNT) {
                peak_slot = WPM_BAR_COUNT - 1;
            }
            int peak_x = start_x + peak_slot * (bar_width + bar_gap) + 2;
            lv_obj_set_pos(widget->peak_indicator, peak_x, 0);
            lv_obj_clear_flag(widget->peak_indicator, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(widget->peak_indicator, LV_OBJ_FLAG_HIDDEN);
        }

        char wpm_text[4];
        snprintf(wpm_text, sizeof(wpm_text), "%d", (int)(displayed_wpm + 0.5f));
        lv_label_set_text(widget->wpm_label, wpm_text);
    }
}

static void wpm_smooth_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    float diff = target_wpm - displayed_wpm;
    bool at_target = (diff > -0.5f && diff < 0.5f);
    int old_int = (int)(displayed_wpm + 0.5f);

    if (at_target) {
        displayed_wpm = target_wpm;
    } else {
        float factor = (diff > 0) ? smoothing_factor_up : smoothing_factor_down;
        displayed_wpm += diff * factor;
    }

    int new_int = (int)(displayed_wpm + 0.5f);
    int active_bars = (new_int * WPM_BAR_COUNT) / WPM_MAX;
    if (active_bars > WPM_BAR_COUNT) {
        active_bars = WPM_BAR_COUNT;
    }

    bool peak_changed = false;
    if (active_bars > peak_position) {
        peak_position = active_bars;
        peak_hold_counter = 0;
        peak_decay_counter = 0;
        peak_changed = true;
    } else if (peak_position > active_bars) {
        if (peak_hold_counter < 90) {
            peak_hold_counter++;
        } else {
            peak_decay_counter++;
            if (peak_decay_counter >= 18) {
                peak_position--;
                peak_decay_counter = 0;
                peak_changed = true;
            }
        }
    }

    if (old_int != new_int || peak_changed) {
        wpm_meter_render(active_bars);
    }

    if (!at_target || peak_position > active_bars) {
        k_work_schedule(&wpm_smooth_work, K_MSEC(33));
    }
}

static void wpm_meter_update_cb(struct wpm_meter_state state) {
    target_wpm = (float)state.wpm;
    k_work_schedule(&wpm_smooth_work, K_NO_WAIT);
}

static struct wpm_meter_state wpm_meter_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    return (struct wpm_meter_state){.wpm = (uint8_t)zmk_wpm_get_state()};
}

ZMK_DISPLAY_WIDGET_LISTENER(dongle_wpm, struct wpm_meter_state,
                            wpm_meter_update_cb, wpm_meter_get_state)
ZMK_SUBSCRIPTION(dongle_wpm, zmk_wpm_state_changed)

/* ───────────────────────────── Layer state ─────────────────────────── */

struct layer_state {
    uint8_t index;
};

static void layer_update_cb(struct layer_state state) {
    struct zmk_widget_wpm_meter *wpm_widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&wpm_widgets, wpm_widget, node) {
        const char *layer_name =
            zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(state.index));
        char display_name[32];

        if (layer_name != NULL && *layer_name != '\0') {
            snprintf(display_name, sizeof(display_name), "%s", layer_name);
        } else {
            snprintf(display_name, sizeof(display_name), "Layer %d", state.index);
        }

        for (int i = 0; display_name[i] != '\0'; i++) {
            display_name[i] = (char)toupper((unsigned char)display_name[i]);
        }

        lv_label_set_text(wpm_widget->layer_label, display_name);
    }

    struct layer_dots_widget *dots_widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&layer_dots_widgets, dots_widget, node) {
        for (int i = 0; i < LAYER_DOT_COUNT; i++) {
            lv_color_t color = (i == state.index)
                                   ? lv_color_hex(DISPLAY_COLOR_LAYER_DOT_ACTIVE)
                                   : lv_color_hex(DISPLAY_COLOR_LAYER_DOT_INACTIVE);
            lv_obj_set_style_bg_color(dots_widget->dots[i], color, LV_PART_MAIN);
        }
    }
}

static struct layer_state layer_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    return (struct layer_state){
        .index = (uint8_t)zmk_keymap_highest_layer_active(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(dongle_layer, struct layer_state,
                            layer_update_cb, layer_get_state)
ZMK_SUBSCRIPTION(dongle_layer, zmk_layer_state_changed)

int zmk_widget_wpm_meter_init(struct zmk_widget_wpm_meter *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(widget->obj, 260, 90);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);

    int bar_width = 8;
    int bar_gap = 2;
    int bar_height = 90;
    int total_width = WPM_BAR_COUNT * bar_width + (WPM_BAR_COUNT - 1) * bar_gap;
    int start_x = (260 - total_width) / 2;

    for (int i = 0; i < WPM_BAR_COUNT; i++) {
        widget->bars[i] = lv_obj_create(widget->obj);
        lv_obj_clear_flag(widget->bars[i], LV_OBJ_FLAG_SCROLLABLE |
                                             LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(widget->bars[i], bar_width, bar_height);
        lv_obj_set_pos(widget->bars[i], start_x + i * (bar_width + bar_gap), 0);
        lv_obj_set_style_bg_color(widget->bars[i],
                                  lv_color_hex(DISPLAY_COLOR_WPM_BAR_INACTIVE),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(widget->bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(widget->bars[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(widget->bars[i], 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(widget->bars[i], 0, LV_PART_MAIN);
    }

    widget->peak_indicator = lv_obj_create(widget->obj);
    lv_obj_clear_flag(widget->peak_indicator, LV_OBJ_FLAG_SCROLLABLE |
                                                  LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(widget->peak_indicator, 4, bar_height);
    lv_obj_set_style_bg_color(widget->peak_indicator, lv_color_hex(0x505050),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->peak_indicator, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->peak_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->peak_indicator, 1, LV_PART_MAIN);
    lv_obj_add_flag(widget->peak_indicator, LV_OBJ_FLAG_HIDDEN);

    widget->wpm_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->wpm_label, "0");
    lv_obj_set_style_text_font(widget->wpm_label, &FR_Medium_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(widget->wpm_label,
                                lv_color_hex(DISPLAY_COLOR_WPM_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->wpm_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->wpm_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(widget->wpm_label, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(widget->wpm_label, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_top(widget->wpm_label, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(widget->wpm_label, 4, LV_PART_MAIN);
    lv_obj_align(widget->wpm_label, LV_ALIGN_TOP_LEFT, -7, -9);

    widget->layer_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->layer_label, "");
    lv_obj_set_style_text_font(widget->layer_label, &DINishExpanded_Light_36,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(widget->layer_label,
                                lv_color_hex(DISPLAY_COLOR_LAYER_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->layer_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->layer_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(widget->layer_label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(widget->layer_label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(widget->layer_label, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(widget->layer_label, 3, LV_PART_MAIN);
    lv_obj_align(widget->layer_label, LV_ALIGN_BOTTOM_RIGHT, 9, 7);

    sys_slist_append(&wpm_widgets, &widget->node);

    if (!wpm_work_initialized) {
        k_work_init_delayable(&wpm_smooth_work, wpm_smooth_work_handler);
        wpm_work_initialized = true;
    }
    dongle_wpm_init();

    return 0;
}

int zmk_widget_layer_dots_init(struct layer_dots_widget *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(widget->obj, 260, 6);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);

    int dot_gap = 3;
    int dot_width = (260 - (LAYER_DOT_COUNT - 1) * dot_gap) / LAYER_DOT_COUNT;
    for (int i = 0; i < LAYER_DOT_COUNT; i++) {
        widget->dots[i] = lv_obj_create(widget->obj);
        lv_obj_clear_flag(widget->dots[i], LV_OBJ_FLAG_SCROLLABLE |
                                             LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(widget->dots[i], dot_width, 6);
        lv_obj_set_pos(widget->dots[i], i * (dot_width + dot_gap), 0);
        lv_obj_set_style_bg_color(widget->dots[i],
                                  lv_color_hex(DISPLAY_COLOR_LAYER_DOT_INACTIVE),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(widget->dots[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(widget->dots[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(widget->dots[i], 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(widget->dots[i], 0, LV_PART_MAIN);
    }

    sys_slist_append(&layer_dots_widgets, &widget->node);
    dongle_layer_init();
    return 0;
}

/* ─────────────────────────── Modifier indicators ────────────────────── */

struct modifier_indicator_state {
    bool mods[4];
};

static void modifier_indicator_update_cb(struct modifier_indicator_state state) {
    struct zmk_widget_modifier_indicator *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&modifier_widgets, widget, node) {
        for (int i = 0; i < 4; i++) {
            lv_color_t color = state.mods[i]
                                   ? lv_color_hex(DISPLAY_COLOR_MOD_ACTIVE)
                                   : lv_color_hex(DISPLAY_COLOR_MOD_INACTIVE);
            lv_obj_set_style_text_color(widget->mod_labels[i], color, LV_PART_MAIN);
        }
    }
}

static struct modifier_indicator_state modifier_indicator_get_state(
    const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    zmk_mod_flags_t mods = zmk_hid_get_explicit_mods();
    return (struct modifier_indicator_state){
        .mods = {
            (mods & (MOD_LGUI | MOD_RGUI)) != 0,
            (mods & (MOD_LALT | MOD_RALT)) != 0,
            (mods & (MOD_LCTL | MOD_RCTL)) != 0,
            (mods & (MOD_LSFT | MOD_RSFT)) != 0,
        },
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(dongle_modifiers, struct modifier_indicator_state,
                            modifier_indicator_update_cb,
                            modifier_indicator_get_state)
ZMK_SUBSCRIPTION(dongle_modifiers, zmk_keycode_state_changed)

int zmk_widget_modifier_indicator_init(
    struct zmk_widget_modifier_indicator *widget, lv_obj_t *parent) {
    static const char *const modifier_texts[] = {"GUI", "ALT", "CTRL", "SHIFT"};

    widget->obj = lv_obj_create(parent);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(widget->obj, 230, 24);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_layout(widget->obj, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(widget->obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(widget->obj, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 4; i++) {
        widget->mod_labels[i] = lv_label_create(widget->obj);
        lv_label_set_text(widget->mod_labels[i], modifier_texts[i]);
        lv_obj_set_style_text_font(widget->mod_labels[i], &FG_Medium_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(widget->mod_labels[i],
                                    lv_color_hex(DISPLAY_COLOR_MOD_INACTIVE),
                                    LV_PART_MAIN);
    }

    sys_slist_append(&modifier_widgets, &widget->node);
    dongle_modifiers_init();
    return 0;
}

/* ───────────────────────────── Battery rings ────────────────────────── */

static lv_obj_t *peripheral_arcs[PERIPHERAL_COUNT];
static lv_obj_t *peripheral_label_boxes[PERIPHERAL_COUNT];
static lv_obj_t *peripheral_labels[PERIPHERAL_COUNT];
static uint8_t peripheral_battery[PERIPHERAL_COUNT];
static bool peripheral_connected[PERIPHERAL_COUNT];

struct battery_update_state {
    uint8_t source;
    uint8_t level;
};

struct connection_update_state {
    uint8_t source;
    bool connected;
};

static void update_peripheral_display(uint8_t source) {
    if (source >= PERIPHERAL_COUNT || peripheral_arcs[source] == NULL) {
        return;
    }

    lv_obj_t *arc = peripheral_arcs[source];
    lv_obj_t *label_box = peripheral_label_boxes[source];
    lv_obj_t *label = peripheral_labels[source];
    bool connected = peripheral_connected[source];
    uint8_t level = peripheral_battery[source];
    bool low_battery = connected && level > 0 && level <= LOW_BATTERY_THRESHOLD;

    uint32_t ring_color = DISPLAY_COLOR_BATTERY_DISCONNECTED_RING;
    uint32_t fill_color = DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL;
    if (low_battery) {
        ring_color = DISPLAY_COLOR_BATTERY_LOW_RING;
        fill_color = DISPLAY_COLOR_BATTERY_LOW_FILL;
    } else if (connected) {
        ring_color = DISPLAY_COLOR_BATTERY_RING;
        fill_color = DISPLAY_COLOR_BATTERY_FILL;
    }

    lv_obj_set_style_arc_color(arc, lv_color_hex(ring_color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(fill_color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, connected ? 6 : 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, connected ? 6 : 2, LV_PART_INDICATOR);
    lv_arc_set_value(arc, connected ? level : 0);

    lv_obj_set_style_bg_color(label_box, lv_color_hex(fill_color), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    if (connected && level > 0) {
        lv_label_set_text_fmt(label, "%d", level);
    } else {
        lv_label_set_text(label, "-");
    }
}

static void battery_update_cb(struct battery_update_state state) {
    if (state.source >= PERIPHERAL_COUNT) {
        return;
    }

    peripheral_battery[state.source] = state.level;
    update_peripheral_display(state.source);
}

static struct battery_update_state battery_get_state(const zmk_event_t *eh) {
    if (eh == NULL) {
        return (struct battery_update_state){.source = 0, .level = 0};
    }

    const struct zmk_peripheral_battery_state_changed *event =
        as_zmk_peripheral_battery_state_changed(eh);
    if (event == NULL) {
        return (struct battery_update_state){.source = 0, .level = 0};
    }

    return (struct battery_update_state){
        .source = event->source,
        .level = event->state_of_charge,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(dongle_battery, struct battery_update_state,
                            battery_update_cb, battery_get_state)
ZMK_SUBSCRIPTION(dongle_battery, zmk_peripheral_battery_state_changed)

static void connection_update_cb(struct connection_update_state state) {
    if (state.source >= PERIPHERAL_COUNT) {
        return;
    }

    peripheral_connected[state.source] = state.connected;
    update_peripheral_display(state.source);
}

static struct connection_update_state connection_get_state(const zmk_event_t *eh) {
    if (eh == NULL) {
        return (struct connection_update_state){.source = 0, .connected = false};
    }

    const struct zmk_split_central_status_changed *event =
        as_zmk_split_central_status_changed(eh);
    if (event == NULL) {
        return (struct connection_update_state){.source = 0, .connected = false};
    }

    return (struct connection_update_state){
        .source = event->slot,
        .connected = event->connected,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(dongle_connection, struct connection_update_state,
                            connection_update_cb, connection_get_state)
ZMK_SUBSCRIPTION(dongle_connection, zmk_split_central_status_changed)

int zmk_widget_battery_circles_init(struct zmk_widget_battery_circles *widget,
                                    lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(widget->obj, 132, 62);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);

    int arc_size = 58;
    int y_center = (62 - arc_size) / 2;
    int spacing = 66;
    for (int i = 0; i < PERIPHERAL_COUNT; i++) {
        lv_obj_t *arc = lv_arc_create(widget->obj);
        peripheral_arcs[i] = arc;
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(arc, arc_size, arc_size);
        lv_obj_set_pos(arc, i * spacing, y_center);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 0);
        lv_arc_set_bg_angles(arc, 270, 180);
        lv_arc_set_rotation(arc, 0);
        lv_obj_set_style_arc_width(arc, 2, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 2, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(
            arc, lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_RING), LV_PART_MAIN);
        lv_obj_set_style_arc_color(
            arc, lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL),
            LV_PART_INDICATOR);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);

        peripheral_label_boxes[i] = lv_obj_create(arc);
        lv_obj_t *label_box = peripheral_label_boxes[i];
        lv_obj_clear_flag(label_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(label_box, 25, 25);
        lv_obj_set_pos(label_box, 0, 0);
        lv_obj_set_style_bg_color(
            label_box, lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(label_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(label_box, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(label_box, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(label_box, 0, LV_PART_MAIN);

        peripheral_labels[i] = lv_label_create(label_box);
        lv_label_set_text(peripheral_labels[i], "-");
        lv_obj_set_style_text_font(peripheral_labels[i], &DINish_Medium_24,
                                   LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(peripheral_labels[i], -1, LV_PART_MAIN);
        lv_obj_set_style_text_color(peripheral_labels[i], lv_color_black(),
                                    LV_PART_MAIN);
        lv_obj_align(peripheral_labels[i], LV_ALIGN_CENTER, 0, 0);
    }

    dongle_battery_init();
    dongle_connection_init();
    return 0;
}

/* ─────────────────────────── USB/BLE output ─────────────────────────── */

struct output_state {
    enum zmk_transport transport;
    uint8_t profile_index;
};

static void set_toggle_btn_state(lv_obj_t *button, bool active, bool is_usb) {
    lv_obj_t *label = lv_obj_get_child(button, 0);
    uint32_t active_bg = is_usb ? DISPLAY_COLOR_USB_ACTIVE_BG
                                : DISPLAY_COLOR_BLE_ACTIVE_BG;
    uint32_t inactive_bg = is_usb ? DISPLAY_COLOR_USB_INACTIVE_BG
                                  : DISPLAY_COLOR_BLE_INACTIVE_BG;

    if (active) {
        lv_obj_set_style_bg_color(button, lv_color_hex(active_bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
        if (label != NULL) {
            lv_obj_set_style_text_color(label,
                                        lv_color_hex(DISPLAY_COLOR_OUTPUT_ACTIVE_TEXT),
                                        LV_PART_MAIN);
        }
    } else {
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(button, lv_color_hex(inactive_bg), LV_PART_MAIN);
        if (label != NULL) {
            lv_obj_set_style_text_color(label, lv_color_hex(inactive_bg),
                                        LV_PART_MAIN);
        }
    }
}

static void output_update_cb(struct output_state state) {
    struct zmk_widget_output *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&output_widgets, widget, node) {
        bool is_usb = state.transport == ZMK_TRANSPORT_USB;
        set_toggle_btn_state(widget->usb_btn, is_usb, true);
        set_toggle_btn_state(widget->ble_btn, !is_usb, false);

        for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
            lv_obj_set_style_bg_color(
                widget->slots[i],
                lv_color_hex(i == state.profile_index ? DISPLAY_COLOR_SLOT_ACTIVE_BG
                                                       : DISPLAY_COLOR_SLOT_INACTIVE_BG),
                LV_PART_MAIN);
        }
    }
}

static struct output_state output_get_state(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    struct zmk_endpoint_instance selected = zmk_endpoint_get_selected();
    return (struct output_state){
        .transport = selected.transport,
        .profile_index = (uint8_t)zmk_ble_active_profile_index(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(dongle_output_endpoint, struct output_state,
                            output_update_cb, output_get_state)
ZMK_SUBSCRIPTION(dongle_output_endpoint, zmk_endpoint_changed)

ZMK_DISPLAY_WIDGET_LISTENER(dongle_output_profile, struct output_state,
                            output_update_cb, output_get_state)
ZMK_SUBSCRIPTION(dongle_output_profile, zmk_ble_active_profile_changed)

static lv_obj_t *create_toggle_button(lv_obj_t *parent, const char *text, int x) {
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, 56, 29);
    lv_obj_set_pos(button, x, 0);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &FG_Medium_20, LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_set_style_translate_y(label, 1, LV_PART_MAIN);
    return button;
}

static lv_obj_t *create_profile_button(lv_obj_t *parent, int index, int x,
                                       int width) {
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(button, width, 29);
    lv_obj_set_pos(button, x, 33);
    lv_obj_set_style_radius(button, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(button);
    char text[3];
    snprintf(text, sizeof(text), "%d", index + 1);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &FG_Medium_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_black(), LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_set_style_translate_y(label, 1, LV_PART_MAIN);
    return button;
}

int zmk_widget_output_init(struct zmk_widget_output *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(widget->obj, 116, 62);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_PART_MAIN);

    widget->usb_btn = create_toggle_button(widget->obj, "USB", 0);
    widget->ble_btn = create_toggle_button(widget->obj, "BLE", 58);

    int slot_spacing = 2;
    int slot_width = (116 - (ZMK_BLE_PROFILE_COUNT - 1) * slot_spacing) /
                     ZMK_BLE_PROFILE_COUNT;
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        int x = i * (slot_width + slot_spacing);
        widget->slots[i] = create_profile_button(widget->obj, i, x, slot_width);
    }

    sys_slist_append(&output_widgets, &widget->node);
    dongle_output_endpoint_init();
    dongle_output_profile_init();
    return 0;
}

/* ───────────────────────────── Screen assembly ─────────────────────── */

lv_obj_t *zmk_display_status_screen(void) {
    static struct zmk_widget_modifier_indicator modifier_widget;
    static struct zmk_widget_wpm_meter wpm_widget;
    static struct layer_dots_widget layer_dots_widget;
    static struct zmk_widget_battery_circles battery_widget;
    static struct zmk_widget_output output_widget;

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    zmk_widget_modifier_indicator_init(&modifier_widget, screen);
    lv_obj_set_pos(modifier_widget.obj, 25, 8);

    zmk_widget_wpm_meter_init(&wpm_widget, screen);
    lv_obj_set_pos(wpm_widget.obj, 10, 42);

    zmk_widget_layer_dots_init(&layer_dots_widget, screen);
    lv_obj_set_pos(layer_dots_widget.obj, 10, 142);

    zmk_widget_battery_circles_init(&battery_widget, screen);
    lv_obj_set_pos(battery_widget.obj, 11, 170);

    zmk_widget_output_init(&output_widget, screen);
    lv_obj_set_pos(output_widget.obj, 148, 170);

    return screen;
}
