#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ai_provider_config.h"

namespace hermes_voice {

enum class State { Idle, Recording, Transcribing, Responding, Synthesizing, Speaking };

struct DashboardSession {
    std::string cookie_header;
};

struct GatewayTurnResult {
    std::string stored_session_id;
    std::string text;
};

bool LoginDashboard(const AiProviderConfig& config, DashboardSession* session,
                    std::string* error);
bool CheckDashboardIdentity(const AiProviderConfig& config,
                            const DashboardSession& session, std::string* error);
bool LoadDashboardProfiles(const AiProviderConfig& config,
                           const DashboardSession& session,
                           std::vector<std::string>* profiles, std::string* error);
bool TestDashboardGateway(const AiProviderConfig& config,
                          const DashboardSession& session, std::string* error);
bool RunDashboardTurn(const AiProviderConfig& config,
                      const DashboardSession& session,
                      std::string_view user_text,
                      std::string_view stored_session_id,
                      GatewayTurnResult* result, std::string* error,
                      std::function<bool()> keep_waiting = {});

}  // namespace hermes_voice
