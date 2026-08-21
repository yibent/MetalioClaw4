#include "hermes_voice_session.h"

namespace hermes_voice {
namespace {

bool Fail(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
    return false;
}

}  // namespace

bool LoginDashboard(const AiProviderConfig&, DashboardSession*, std::string* error) {
    return Fail(error, "Hermes is not available on this board");
}

bool CheckDashboardIdentity(const AiProviderConfig&, const DashboardSession&,
                            std::string* error) {
    return Fail(error, "Hermes is not available on this board");
}

bool LoadDashboardProfiles(const AiProviderConfig&, const DashboardSession&,
                           std::vector<std::string>*, std::string* error) {
    return Fail(error, "Hermes is not available on this board");
}

bool TestDashboardGateway(const AiProviderConfig&, const DashboardSession&,
                          std::string* error) {
    return Fail(error, "Hermes is not available on this board");
}

bool RunDashboardTurn(const AiProviderConfig&, const DashboardSession&,
                      std::string_view, std::string_view, GatewayTurnResult*,
                      std::string* error, std::function<bool()>) {
    return Fail(error, "Hermes is not available on this board");
}

}  // namespace hermes_voice
