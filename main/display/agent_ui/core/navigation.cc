#include "navigation.h"

#include "application.h"
#include "status_bar.h"
#include "theme.h"

namespace agent_ui {
namespace {

bool ScreenNeedsVoiceEngineRelease(ScreenId id) {
    switch (id) {
        case ScreenId::OpenClaw:
        case ScreenId::AiImageGen:
        case ScreenId::Translate:
        case ScreenId::Files:
        case ScreenId::ExternalAppHost:
        case ScreenId::Standby:
            return true;
        default:
            return false;
    }
}

}  // namespace

Navigation& Navigation::Get() {
    static Navigation instance;
    return instance;
}

void Navigation::Register(ScreenId id, AppFactory factory) {
    factories_[static_cast<size_t>(id)] = factory;
}

void Navigation::Start() {
    stack_size_ = 0;
    Load(ScreenId::Home, TransitionDirection::Replace, true);
}

void Navigation::Open(ScreenId id) {
    if (id == current_) return;
    if (current_ == ScreenId::Home && id != ScreenId::Home) {
        Application::GetInstance().ForceReturnToIdle();
    }
    Load(id, TransitionDirection::Forward, true);
}

void Navigation::Back() {
    if (stack_size_ <= 1) {
        if (current_ != ScreenId::Home) Load(ScreenId::Home, TransitionDirection::Back, true);
        return;
    }
    --stack_size_;
    const ScreenId target = stack_[stack_size_ - 1];
    Load(target, TransitionDirection::Back, false);
}

void Navigation::ReturnHome() {
    if (current_ == ScreenId::Home) {
        RebuildCurrent();
        return;
    }
    Open(ScreenId::Home);
}

void Navigation::RebuildCurrent() {
    Load(current_, TransitionDirection::Replace, false);
}

void Navigation::Load(ScreenId id, TransitionDirection direction, bool update_stack) {
    const AppFactory factory = factories_[static_cast<size_t>(id)];
    if (factory == nullptr) return;

    lv_obj_t* root = factory();
    if (root == nullptr) return;

    if (update_stack) {
        if (id == ScreenId::Home) {
            stack_size_ = 1;
            stack_[0] = ScreenId::Home;
        } else if (stack_size_ < stack_.size()) {
            stack_[stack_size_++] = id;
        } else {
            stack_[stack_.size() - 1] = id;
        }
    }
    current_ = id;
    lv_obj_t* previous = lv_screen_active();
    Application::GetInstance().SetVoiceUiDesired(id == ScreenId::Home);
    // 设置等轻量页只软停唤醒词。相机 / 文件 / 翻译会再申请硬释放 AFE。
    if (id != ScreenId::Home && ScreenNeedsVoiceEngineRelease(id)) {
        Application::GetInstance().RequestVoiceEngineHardRelease();
    }
    // 回主页时等旧屏真正 delete 后再重建 AFE（仅硬释放后的路径）。
    if (id == ScreenId::Home && previous != nullptr && previous != root) {
        lv_obj_add_event_cb(
            previous,
            [](lv_event_t*) {
                lv_async_call(
                    [](void*) {
                        Application::GetInstance().NotifyUiTransitionFinished();
                    },
                    nullptr);
            },
            LV_EVENT_DELETE, nullptr);
    }
    StatusBar::Get().SetHomeActive(id == ScreenId::Home);
    const bool show_status_bar = id != ScreenId::OpenClaw &&
                                 id != ScreenId::AiImageGen &&
                                 id != ScreenId::Translate;
    StatusBar::Get().SetVisible(show_status_bar);

    lv_screen_load_anim_t animation = LV_SCR_LOAD_ANIM_NONE;
    if (direction == TransitionDirection::Forward) {
        animation = LV_SCR_LOAD_ANIM_MOVE_LEFT;
    } else if (direction == TransitionDirection::Back) {
        animation = LV_SCR_LOAD_ANIM_MOVE_RIGHT;
    } else {
        animation = LV_SCR_LOAD_ANIM_FADE_IN;
    }
    lv_screen_load_anim(root, animation, metrics::kTransitionMs, 0, true);
}

}  // namespace agent_ui
