#pragma once

namespace LuaAgentSession {

// Connects the Lua Agent WebSocket in the background of the chat home
// screen. Server `run` / `speak` messages execute on the local runtime, and
// script output is posted as assistant chat messages.
void Start();
void Stop();
bool IsRunning();

}  // namespace LuaAgentSession
