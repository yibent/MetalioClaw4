#pragma once

#include <cstdint>

#include "agent_ui_types.h"
#include "config.h"
#include "lvgl.h"

namespace agent_ui {

struct ThemeColors {
    uint32_t background;
    uint32_t surface;
    uint32_t raised;
    uint32_t border;
    uint32_t text;
    uint32_t muted;
    uint32_t accent;
    uint32_t accent_pressed;
    uint32_t accent_ink;
    uint32_t danger;
    uint32_t warning;
};

class Theme {
public:
    static Theme& Get();

    void Initialize();
    AccentPreset accent_preset() const { return accent_preset_; }
    void SetAccentPreset(AccentPreset preset);
    AppearanceMode appearance_mode() const { return appearance_mode_; }
    void SetAppearanceMode(AppearanceMode mode);

    const ThemeColors& colors() const { return colors_; }
    lv_color_t Color(uint32_t value) const { return lv_color_hex(value); }

private:
    Theme() = default;
    void ApplyPreset(AccentPreset preset);

    AccentPreset accent_preset_ = AccentPreset::Coral;
    AppearanceMode appearance_mode_ = AppearanceMode::Dark;
    ThemeColors colors_{};
};

namespace metrics {
// Design reference is the 720x720 Agent UI. Fangtang is 480x800 portrait;
// metalio-claw-4 stays 720x720. Scale uniformly by width, then use the extra
// portrait height for content below the status bar / above the action bar.
#ifndef DISPLAY_WIDTH
#define DISPLAY_WIDTH 720
#endif
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT 720
#endif

constexpr int kRef = 720;
constexpr int kDisplayWidth = DISPLAY_WIDTH;
constexpr int kDisplayHeight = DISPLAY_HEIGHT;
constexpr int kDisplaySize = kDisplayWidth;

constexpr int Scale(int value) {
    return (value * kDisplayWidth + kRef / 2) / kRef;
}

// Fangtang 480x800 uses a 40px bar; 720x720 keeps the 62px design height.
constexpr int kStatusBarHeight = kDisplayWidth == 480 ? 40 : Scale(62);
constexpr int kAppHeaderHeight = Scale(98);
constexpr int kContentTop = kStatusBarHeight + kAppHeaderHeight;
constexpr int kBottomActionBarHeight = Scale(132);
constexpr int kBottomActionBarY = kDisplayHeight - kBottomActionBarHeight;
constexpr int kBottomActionContentHeight =
    kBottomActionBarY - kStatusBarHeight;
constexpr int kRadiusSmall = Scale(8);
constexpr int kRadiusControl = Scale(14);
constexpr int kRadiusPanel = Scale(15);
constexpr int kRadius = kRadiusSmall;
constexpr int kPagePadding = Scale(34);
constexpr int kHeaderPadding = Scale(30);
constexpr int kSystemPadding = Scale(42);
constexpr int kTouchTarget = Scale(56);
constexpr int kBottomActionHeight = Scale(80);
constexpr int kBottomPrimaryActionHeight = Scale(104);
constexpr int kTransitionMs = 180;
}  // namespace metrics

void StyleRoot(lv_obj_t* root);
void StyleSurface(lv_obj_t* object, bool raised = false);
void StyleButton(lv_obj_t* button, bool accent = false);
void StyleTextInput(lv_obj_t* textarea);
void StyleLabel(lv_obj_t* label, bool muted = false);

}  // namespace agent_ui
