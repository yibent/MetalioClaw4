#include "classic_apps_view.h"

#include "core/navigation.h"
#include "core/status_bar.h"
#include "home_screen.h"
#include "screen_util.h"

namespace agent_ui {

lv_obj_t* ClassicAppsView::Create() {
    StatusBar::Get().SetVisible(false);
    HomeScreen::SetHostBackCallback([]() { Navigation::Get().Back(); });
    lv_obj_t* screen = HomeScreen::Create();
    screen_mark_native_layout(screen);
    return screen;
}

}  // namespace agent_ui
