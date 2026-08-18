#pragma once

#include "lvgl.h"

class Calculator {
public:
    // Create a fullscreen calculator app in the active display's native size.
    // Returns a new LVGL screen object (parent = NULL); the caller is
    // responsible for loading it via lv_screen_load().
    // The page contains a "返回" button that navigates back to HomeScreen.
    static lv_obj_t* Create();
};
