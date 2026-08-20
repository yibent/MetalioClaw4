#pragma once

#include "lvgl.h"

class BootScreen {
public:
    // Create a fullscreen boot page with a centered image on a white background.
    // Returns the created LVGL screen object (parent = NULL).
    static lv_obj_t* Create();
};
