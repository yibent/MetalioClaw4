#include "openclaw_view.h"

#include "openclaw_screen.h"
#include "pwr_key_handler.h"

namespace agent_ui {
namespace {

void OpenClawLifecycle(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("openclaw", event);
    OpenClawScreen::LifecycleCallback(event);
}

}  // namespace

lv_obj_t* OpenClawView::Create() {
    OpenClawScreen::SetLifecycleCallback(OpenClawLifecycle);
    return OpenClawScreen::Create();
}

}  // namespace agent_ui
