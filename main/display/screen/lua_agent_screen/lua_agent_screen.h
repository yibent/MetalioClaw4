#pragma once

#include "screen_util.h"

class LuaAgentApp {
public:
    static void Launch(screen_lifecycle_cb_t lifecycle_cb);
    static void LifecycleCallback(screen_lifecycle_event_t event);
};
