#include "ai_image_gen_view.h"

#include "ai_image_gen_screen.h"
#include "pwr_key_handler.h"
#include "screen_util.h"

namespace agent_ui {
namespace {

void AiImageGenLifecycle(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("ai_image_gen", event);
    AiImageGenScreen::LifecycleCallback(event);
}

}  // namespace

lv_obj_t* AiImageGenView::Create() {
    lv_obj_t* screen = AiImageGenScreen::Create();
    screen_attach_lifecycle(screen, AiImageGenLifecycle);
    return screen;
}

}  // namespace agent_ui
