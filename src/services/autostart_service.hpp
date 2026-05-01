#pragma once

// autostart_service — manage the Windows Task Scheduler entry that starts
// clew.exe when the user logs on.
//
// The service is intentionally stateless and exposes only static operations.
// It uses Task Scheduler rather than the HKCU Run key because clew.exe needs
// elevation for WinDivert / SetInterfaceDnsSettings. A Run-key entry would run
// as the unelevated user and trigger UAC on every login; a scheduled task with
// /RL HIGHEST can run with the user's elevated token without prompting.
//
// The scheduled task does not set a working directory. The registered command
// line pins --config to clew.json's absolute path; clew.exe resolves the rest
// of its sibling resources (clew.log, frontend/dist, dns_state.json) against
// its own image path via core/exe_paths.hpp, so cwd is irrelevant.

#include <nlohmann/json.hpp>

namespace clew {

struct autostart_state {
    bool enabled         = false;
    bool start_minimized = false;
};

class autostart_service {
public:
    // Query the ClewAutoStart scheduled task.
    // Returns {enabled=false} when the task is absent. If present, the task XML
    // is inspected to decide whether --minimized is part of the registered
    // command line.
    [[nodiscard]] static autostart_state query();

    // Create, replace, or delete the ClewAutoStart scheduled task.
    //
    // enabled=true:
    //   Creates or replaces the task. The registered command line pins both the
    //   absolute clew.exe path and the absolute clew.json path.
    //
    // enabled=false:
    //   Deletes the task. start_minimized is ignored. Deleting an already absent
    //   task is treated as success.
    //
    // Throws api_exception{io_error} when schtasks.exe cannot be started or
    // returns an unexpected failure.
    static void set(bool enabled, bool start_minimized);
};

[[nodiscard]] inline nlohmann::json to_json(const autostart_state& s) {
    return nlohmann::json{
        {"enabled",         s.enabled},
        {"start_minimized", s.start_minimized},
    };
}

} // namespace clew
