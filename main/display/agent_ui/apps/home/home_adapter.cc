#include "home_adapter.h"

#include "application.h"

namespace agent_ui::home {

bool Adapter::Execute(const Command& command) {
    if (command.type == CommandType::ToggleListening) {
        return Application::GetInstance().ToggleChatState();
    }
    return true;
}

}  // namespace agent_ui::home
