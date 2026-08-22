#include "translate_view.h"

#include "pwr_key_handler.h"
#include "screen_util.h"
#include "translate_screen.h"

namespace agent_ui {
namespace {

void TranslateLifecycle(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("translate", event);
    TranslateScreen::LifecycleCallback(event);
}

}  // namespace

lv_obj_t* TranslateView::Create() {
    lv_obj_t* screen = TranslateScreen::Create();
    screen_attach_lifecycle(screen, TranslateLifecycle);
    return screen;
}

}  // namespace agent_ui
