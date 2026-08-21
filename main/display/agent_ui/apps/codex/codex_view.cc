#include "codex_view.h"

#include "core/app_shell.h"
#include "core/fonts.h"
#include "core/theme.h"

namespace agent_ui {

lv_obj_t* CodexView::Create() {
    AppShell shell = CreateAppShell("Codex", nullptr, true);
    const auto& colors = Theme::Get().colors();
    lv_obj_t* label = lv_label_create(shell.content);
    lv_label_set_text(label, "Codex 尚未接入此板型");
    lv_obj_set_style_text_font(label, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    return shell.root;
}

}  // namespace agent_ui
