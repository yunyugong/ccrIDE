#pragma once
#include "execution.h"
#include <nlohmann/json.hpp>

namespace service {
struct Reply { bool ok=false; std::string error; };
// One request per process. Invoke from a worker, never the UI thread.
// Cancellation/timeout kills the service Job. This is fault containment, not a sandbox.
Reply Call(const std::wstring& executable,const std::string& method,const nlohmann::json& payload,
           std::atomic<bool>& cancel,const std::function<void(const std::string&)>& onText,
           DWORD timeoutMs=120000);
}
