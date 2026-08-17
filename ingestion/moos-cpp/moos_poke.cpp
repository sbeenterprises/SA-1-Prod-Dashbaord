// moos_poke — minimal MOOSDB poke utility for tests and commissioning.
//
//   moos_poke [--host=H] [--port=P] VAR=VAL [VAR=VAL ...]
//
// VAL parses as a double when fully numeric, otherwise posts as a string.
// Force a string with VAR:=VAL (e.g. INDEX:=7 posts the string "7").
// Used by tools/smoke_test.sh to replay the Alpha mission variable
// sequence (WAYPOINT/COMMS_LOST/MISSION_ARMED/OPR_BREACHED/...) against a
// live MOOSDB + grafana_bridge; also handy during commissioning:
//   moos_poke WAYPOINT=true DIST_TO_NEXT_WPT=350

#include "MOOS/libMOOS/Comms/MOOSAsyncCommClient.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

int main(int argc, char** argv) {
    std::string host = "localhost";
    int port = 9000;
    std::vector<std::pair<std::string, std::string>> pokes;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--host=", 0) == 0) {
            host = a.substr(7);
        } else if (a.rfind("--port=", 0) == 0) {
            port = std::atoi(a.c_str() + 7);
        } else {
            const size_t eq = a.find('=');
            if (eq == std::string::npos) {
                std::cerr << "moos_poke: ignoring '" << a
                          << "' (expected VAR=VAL)\n";
                continue;
            }
            pokes.emplace_back(a.substr(0, eq), a.substr(eq + 1));
        }
    }
    if (pokes.empty()) {
        std::cerr << "usage: moos_poke [--host=H] [--port=P] VAR=VAL ...\n";
        return 1;
    }

    MOOS::MOOSAsyncCommClient client;
    client.SetQuiet(true);
    client.Run(host, port, "moos_poke");
    for (int i = 0; i < 50 && !client.IsConnected(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!client.IsConnected()) {
        std::cerr << "moos_poke: no MOOSDB at " << host << ":" << port
                  << "\n";
        return 1;
    }

    for (const auto& [var, val] : pokes) {
        const bool force_str = !var.empty() && var.back() == ':';
        const std::string name =
            force_str ? var.substr(0, var.size() - 1) : var;
        char* end = nullptr;
        const double d = std::strtod(val.c_str(), &end);
        if (!force_str && !val.empty() && end && *end == '\0')
            client.Notify(name, d);
        else
            client.Notify(name, val);
    }
    // Give the async client a beat to drain the outbox before closing.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    client.Close(true);
    return 0;
}
