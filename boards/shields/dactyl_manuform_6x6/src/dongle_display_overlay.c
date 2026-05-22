#include <lvgl.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

static struct k_work_delayable display_overlay_work;
static bool display_overlay_installed;

static lv_point_t left_ear_points[] = {{18, 34}, {36, 8}, {54, 34}};
static lv_point_t right_ear_points[] = {{58, 34}, {76, 8}, {94, 34}};
static lv_point_t left_whisker_top[] = {{30, 56}, {6, 50}};
static lv_point_t left_whisker_mid[] = {{30, 62}, {4, 62}};
static lv_point_t left_whisker_bottom[] = {{30, 68}, {6, 74}};
static lv_point_t right_whisker_top[] = {{82, 56}, {106, 50}};
static lv_point_t right_whisker_mid[] = {{82, 62}, {108, 62}};
static lv_point_t right_whisker_bottom[] = {{82, 68}, {106, 74}};
static lv_point_t mouth_left[] = {{56, 66}, {50, 72}, {42, 70}};
static lv_point_t mouth_right[] = {{56, 66}, {62, 72}, {70, 70}};

static void style_line(lv_obj_t *line, lv_color_t color, lv_coord_t width) {
    lv_obj_set_style_line_color(line, color, LV_PART_MAIN);
    lv_obj_set_style_line_width(line, width, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
}

static lv_obj_t *add_line(lv_obj_t *parent, lv_point_t *points, uint16_t point_count,
                          lv_color_t color, lv_coord_t width) {
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, point_count);
    style_line(line, color, width);
    return line;
}

static void add_circle(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t size,
                       lv_color_t color) {
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_set_size(circle, size, size);
    lv_obj_set_pos(circle, x, y);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circle, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(circle, 0, LV_PART_MAIN);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
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

static void create_cat(lv_obj_t *screen) {
    lv_color_t white = lv_color_hex(0xf7f7f7);
    lv_color_t gray = lv_color_hex(0x909090);
    lv_color_t black = lv_color_hex(0x000000);
    lv_color_t pink = lv_color_hex(0xff8fb3);

    lv_obj_t *cat = lv_obj_create(screen);
    lv_obj_set_size(cat, 112, 92);
    lv_obj_align(cat, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_bg_opa(cat, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cat, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cat, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cat, LV_OBJ_FLAG_SCROLLABLE);

    add_line(cat, left_ear_points, 3, white, 3);
    add_line(cat, right_ear_points, 3, white, 3);

    lv_obj_t *head = lv_obj_create(cat);
    lv_obj_set_size(head, 82, 58);
    lv_obj_set_pos(head, 15, 30);
    lv_obj_set_style_radius(head, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(head, black, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(head, white, LV_PART_MAIN);
    lv_obj_set_style_border_width(head, 3, LV_PART_MAIN);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    add_circle(cat, 36, 51, 8, white);
    add_circle(cat, 70, 51, 8, white);
    add_circle(cat, 53, 61, 7, pink);

    add_line(cat, left_whisker_top, 2, gray, 2);
    add_line(cat, left_whisker_mid, 2, gray, 2);
    add_line(cat, left_whisker_bottom, 2, gray, 2);
    add_line(cat, right_whisker_top, 2, gray, 2);
    add_line(cat, right_whisker_mid, 2, gray, 2);
    add_line(cat, right_whisker_bottom, 2, gray, 2);
    add_line(cat, mouth_left, 3, white, 2);
    add_line(cat, mouth_right, 3, white, 2);
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
    create_cat(screen);
    display_overlay_installed = true;
}

static void display_overlay_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    lv_async_call(install_display_overlay, NULL);
}

static int display_overlay_init(const struct device *dev) {
    ARG_UNUSED(dev);

    k_work_init_delayable(&display_overlay_work, display_overlay_work_handler);
    k_work_schedule(&display_overlay_work, K_SECONDS(2));

    return 0;
}

SYS_INIT(display_overlay_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
