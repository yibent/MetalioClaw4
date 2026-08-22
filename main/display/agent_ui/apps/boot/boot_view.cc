#include "boot_view.h"

#include <algorithm>
#include <cstdio>

#include "components/expression_player.h"
#include "core/fonts.h"
#include "core/theme.h"

namespace agent_ui {
namespace {

constexpr int32_t kProgressWidth = metrics::Scale(636);
constexpr bool kPortraitHome =
    metrics::kDisplayHeight > metrics::kDisplayWidth;
constexpr int kExpressionViewWidth =
    kPortraitHome ? metrics::kDisplayWidth : metrics::Scale(600);
constexpr int kExpressionViewHeight = metrics::Scale(400);
constexpr int kExpressionViewX = kPortraitHome ? 0 : metrics::Scale(60);
constexpr int kExpressionViewY = 200;
constexpr int kBootCopyY = 460;
lv_obj_t* s_title = nullptr;
lv_obj_t* s_detail = nullptr;
lv_obj_t* s_progress = nullptr;

void DeleteExpressionPlayer(lv_event_t* event) {
    auto* player =
        static_cast<ExpressionPlayer*>(lv_event_get_user_data(event));
    delete player;
    s_title = nullptr;
    s_detail = nullptr;
    s_progress = nullptr;
}

}  // namespace

lv_obj_t* BootView::Create() {
    const auto& colors = Theme::Get().colors();
    lv_obj_t* screen = lv_obj_create(nullptr);
    StyleRoot(screen);

    lv_obj_t* expression = lv_obj_create(screen);
    lv_obj_remove_style_all(expression);
    lv_obj_set_size(expression, kExpressionViewWidth, kExpressionViewHeight);
    lv_obj_set_pos(expression, kExpressionViewX, kExpressionViewY);
    lv_obj_set_style_bg_opa(expression, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(expression, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(expression, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(expression, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    auto* expression_player = new ExpressionPlayer(expression);
    expression_player->PlayBootAnimation();
    lv_obj_add_event_cb(screen, DeleteExpressionPlayer, LV_EVENT_DELETE,
                        expression_player);

    const int title_height = fonts::LargeBold()->line_height;
    const int detail_height = fonts::Medium()->line_height;
    constexpr int kRuleHeight = 4;
    constexpr int kTitleGap = 22;
    constexpr int kDetailGap = 12;
    constexpr int kTrackGap = 24;
    constexpr int kTrackHeight = 12;
    const int copy_height = kRuleHeight + kTitleGap + title_height + kDetailGap +
                            detail_height + kTrackGap + kTrackHeight;

    lv_obj_t* copy = lv_obj_create(screen);
    lv_obj_remove_style_all(copy);
    lv_obj_set_size(copy, metrics::Scale(636), copy_height);
    lv_obj_set_pos(copy, metrics::kSystemPadding, kBootCopyY);
    lv_obj_remove_flag(copy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(copy, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_move_foreground(copy);

    lv_obj_t* rule = lv_obj_create(copy);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 58, kRuleHeight);
    lv_obj_set_style_bg_color(rule, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(rule, 0, 0);

    s_title = lv_label_create(copy);
    lv_label_set_text(s_title, "正在启动");
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_title, fonts::LargeBold(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_title, lv_color_hex(colors.text), LV_PART_MAIN);
    lv_obj_set_height(s_title, title_height);
    lv_obj_align_to(s_title, rule, LV_ALIGN_OUT_BOTTOM_LEFT, 0, kTitleGap);

    s_detail = lv_label_create(copy);
    lv_label_set_text(s_detail, "正在检查外部 App");
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_detail, fonts::Medium(), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(colors.muted), LV_PART_MAIN);
    lv_obj_set_height(s_detail, detail_height);
    lv_obj_align_to(s_detail, s_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, kDetailGap);

    lv_obj_t* track = lv_obj_create(copy);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, kProgressWidth, kTrackHeight);
    lv_obj_set_style_bg_color(track, lv_color_hex(colors.raised), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(track, 0, LV_PART_MAIN);
    lv_obj_align_to(track, s_detail, LV_ALIGN_OUT_BOTTOM_LEFT, 0, kTrackGap);

    s_progress = lv_obj_create(track);
    lv_obj_remove_style_all(s_progress);
    lv_obj_set_size(s_progress, 0, 12);
    lv_obj_set_style_bg_color(s_progress, lv_color_hex(colors.accent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress, 0, LV_PART_MAIN);
    lv_obj_align(s_progress, LV_ALIGN_LEFT_MID, 0, 0);

    return screen;
}

void BootView::SetInstallProgress(const char* app_name, size_t package_index,
                                  size_t package_count, uint8_t percent) {
    if (s_title == nullptr || s_detail == nullptr || s_progress == nullptr) return;
    const uint8_t clamped_percent = std::min<uint8_t>(percent, 100);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%s  ·  %u/%u  ·  %u%%",
                  app_name != nullptr && app_name[0] != '\0' ? app_name : "外部 App",
                  static_cast<unsigned>(package_index),
                  static_cast<unsigned>(package_count),
                  static_cast<unsigned>(clamped_percent));
    lv_label_set_text(s_title, "正在安装 App");
    lv_label_set_text(s_detail, detail);
    lv_obj_set_width(
        s_progress,
        (kProgressWidth * static_cast<int32_t>(clamped_percent)) / 100);
}

void BootView::SetReady() {
    if (s_title == nullptr || s_detail == nullptr || s_progress == nullptr) return;
    lv_label_set_text(s_title, "正在启动");
    lv_label_set_text(s_detail, "App 安装完成，正在载入桌面");
    lv_obj_set_width(s_progress, kProgressWidth);
}

}  // namespace agent_ui
