#include "boot_screen.h"

namespace {
constexpr const char* kBootImagePath = "A:ic_boot_image.spng";
}

lv_obj_t* BootScreen::Create() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* image = lv_image_create(screen);
    lv_image_set_src(image, kBootImagePath);
    lv_obj_center(image);

    return screen;
}
