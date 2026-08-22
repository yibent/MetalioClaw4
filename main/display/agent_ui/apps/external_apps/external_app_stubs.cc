#include "external_app_manager.h"
#include "external_apps_view.h"

#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/theme.h"

namespace agent_ui::external_apps {

Manager& Manager::Get() {
    static Manager instance;
    return instance;
}

bool Manager::Refresh(std::string* error, const InstallProgressCallback&) {
    apps_.clear();
    selected_id_.clear();
    if (error != nullptr) error->clear();
    return true;
}

bool Manager::Select(const std::string& id) {
    selected_id_ = id;
    return false;
}

const AppInfo* Manager::selected_app() const { return nullptr; }

lv_obj_t* HostView::Create() {
    AppShell shell = CreateAppShell("外部应用", nullptr, true);
    const auto& colors = Theme::Get().colors();
    lv_obj_t* label = lv_label_create(shell.content);
    lv_label_set_text(label, "当前固件未启用外部 App 运行时");
    lv_obj_set_style_text_font(label, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    return shell.root;
}

}  // namespace agent_ui::external_apps
