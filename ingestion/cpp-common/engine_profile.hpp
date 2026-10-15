// Per-vessel engine profile shared by the C++ bridges.
//
// One release serves every hull; what differs is /etc/usv/usv.env:
//   ENGINE_TYPE      yamaha_outboard | mercury_diesel | ...   -> engine_type tag
//   ENGINE_SOURCE    signalk | moos | can                      -> which ONE producer
//                    writes the engine fields (rpm, coolant_temp, ...)
//   ENGINE_COUNT     1 | 2
//   ENGINE_INSTANCES 0 | 0,1
//   HELM_PROFILE     usv722 | usv725 | none   -> can_bridge helm / drive decoder
//   J1939_ENGINE_SA  source address of the J1939 engine ECU (0, the Mercury
//                    ECU on usv-725) read by can_bridge when ENGINE_SOURCE=can
//   SIGNALK_ENGINE, CAN_N2K_ENGINE, CAN_DM1, CAN_J1939_ENGINE -> explicit 0/1
//                    overrides of the switches derived from ENGINE_SOURCE
//                    (normally unset)
//
// The systemd units pass usv.env as EnvironmentFile. grafana_bridge runs
// under pAntler without it, so a key missing from the environment is read
// from /etc/usv/usv.env directly (USV_ENV_FILE overrides the path).
//
// With ENGINE_SOURCE unset every producer keeps writing, as before this
// profile existed; a set value turns the others off so each engine field has
// exactly one writer per vessel (docs/schema.md, "engine").
#pragma once

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace usv {

// KEY=VALUE lines of a systemd EnvironmentFile. Blank lines and lines
// starting with # are skipped, surrounding quotes are removed, a trailing
// "   # comment" after an unquoted value is dropped.
inline std::map<std::string, std::string> ReadEnvFile(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        size_t eq = line.find('=', a);
        if (eq == std::string::npos) continue;
        std::string key = line.substr(a, eq - a);
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        std::string val = line.substr(eq + 1);
        size_t b = val.find_first_not_of(" \t");
        val = (b == std::string::npos) ? "" : val.substr(b);
        if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
            size_t end = val.find(val[0], 1);
            val = val.substr(1, end == std::string::npos ? std::string::npos : end - 1);
        } else {
            size_t hash = val.find(" #");
            if (hash == std::string::npos) hash = val.find("\t#");
            if (hash != std::string::npos) val = val.substr(0, hash);
            while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
        }
        out[key] = val;
    }
    return out;
}

// One setting from the process environment, else from /etc/usv/usv.env
// (USV_ENV_FILE overrides the path); "" when neither has it. For programs
// that run outside systemd (grafana_bridge under pAntler).
inline std::string UsvSetting(const std::string& key) {
    if (const char* v = std::getenv(key.c_str())) return v;
    const char* f = std::getenv("USV_ENV_FILE");
    auto m = ReadEnvFile(f ? f : "/etc/usv/usv.env");
    auto it = m.find(key);
    return it == m.end() ? "" : it->second;
}

struct EngineProfile {
    std::string type;          // ENGINE_TYPE, "" when unset
    std::string model;         // ENGINE_MODEL, the name the dashboards show;
                               // defaults to a name for the type
    std::string source;        // signalk | moos | can | "" (unset: every producer writes)
    int count = 1;             // ENGINE_COUNT
    std::string instances;     // ENGINE_INSTANCES as written, e.g. "0" or "0,1"
    std::string helm_profile;  // HELM_PROFILE, "" = usv722 (the decoder shipped so far)

    bool signalk_engine = true;  // signalk_bridge writes engine fields
    bool moos_engine = true;     // grafana_bridge passes ENGINE_* through
    bool can_n2k_engine = true;  // can_bridge decodes N2K 127488 / 127505
    bool can_dm1 = true;         // can_bridge decodes the Glendinning (address
                                 // 210) J1939 DM1 into the engine fault fields
    bool can_j1939_engine = false;  // can_bridge decodes the J1939 engine ECU
                                    // itself (EEC1, ET1, DM1 ...): ENGINE_SOURCE=can
    int j1939_engine_sa = 0;     // J1939_ENGINE_SA, the engine ECU's address

    // Reads the process environment first, then the env file for keys the
    // environment does not have.
    static EngineProfile FromEnv(const std::string& file = "") {
        const char* f = std::getenv("USV_ENV_FILE");
        const std::string path = !file.empty() ? file : (f ? f : "/etc/usv/usv.env");
        std::map<std::string, std::string> fromfile;
        bool loaded = false;
        auto get = [&](const char* key) -> std::string {
            if (const char* v = std::getenv(key)) return v;
            if (!loaded) { fromfile = ReadEnvFile(path); loaded = true; }
            auto it = fromfile.find(key);
            return it == fromfile.end() ? "" : it->second;
        };
        EngineProfile p;
        p.type = Slug(get("ENGINE_TYPE"));
        p.model = Trim(get("ENGINE_MODEL"));
        if (p.model.empty()) p.model = DefaultModel(p.type);
        p.source = Lower(get("ENGINE_SOURCE"));
        if (p.source != "signalk" && p.source != "moos" && p.source != "can") p.source = "";
        const std::string c = get("ENGINE_COUNT");
        p.count = (c == "2") ? 2 : 1;
        p.instances = get("ENGINE_INSTANCES");
        if (p.instances.empty()) p.instances = p.count == 2 ? "0,1" : "0";
        p.helm_profile = Lower(get("HELM_PROFILE"));
        if (p.helm_profile.empty()) p.helm_profile = "usv722";

        if (!p.source.empty()) {
            p.signalk_engine = p.source == "signalk";
            p.moos_engine = p.source == "moos";
            p.can_n2k_engine = p.source == "can";
            // ENGINE_SOURCE=can: can_bridge reads the J1939 engine ECU on
            // the bus directly (usv-725, Mercury). The engine's own DM1 is
            // then the fault source, so the Glendinning DM1 (address 210)
            // must not write the same fault fields.
            p.can_j1939_engine = p.source == "can";
            // Otherwise the Glendinning DM1 on the drive bus is the fault
            // source unless the engine reports faults through MOOS (iCANRec).
            p.can_dm1 = p.source != "moos" && p.source != "can";
        }
        Override(get("SIGNALK_ENGINE"), &p.signalk_engine);
        Override(get("CAN_N2K_ENGINE"), &p.can_n2k_engine);
        Override(get("CAN_DM1"), &p.can_dm1);
        Override(get("CAN_J1939_ENGINE"), &p.can_j1939_engine);
        Override(get("MOOS_ENGINE"), &p.moos_engine);
        const std::string sa = Trim(get("J1939_ENGINE_SA"));
        if (!sa.empty()) {
            try {
                const int v = std::stoi(sa, nullptr, 0);   // 0, 0x00, 23 ...
                if (v >= 0 && v <= 253) p.j1939_engine_sa = v;
            } catch (...) {}
        }
        return p;
    }

    // Line-protocol tags for an engine point: the caller's tags (e.g.
    // "instance=0") plus engine_type and engine_model when the profile
    // names a type. engine_type selects the dial scales; engine_model is the
    // name in the header chip and on the Fleet page.
    std::string Tags(const std::string& base) const {
        if (type.empty()) return base;
        std::string t = "engine_type=" + type;
        if (!model.empty()) t += ",engine_model=" + EscapeTag(model);
        return base.empty() ? t : base + "," + t;
    }

    // Name shown when ENGINE_MODEL is not set.
    static std::string DefaultModel(const std::string& type) {
        if (type == "yamaha_outboard") return "Yamaha outboard";
        if (type == "mercury_diesel") return "Mercury diesel";
        return type;
    }

    // InfluxDB line protocol: spaces, commas and equals signs in a tag
    // value are backslash-escaped.
    static std::string EscapeTag(const std::string& v) {
        std::string out;
        for (char ch : v) {
            if (ch == ' ' || ch == ',' || ch == '=') out += '\\';
            if (ch == '\n' || ch == '\r') continue;
            out += ch;
        }
        return out;
    }

    std::string Describe() const {
        return "engine_type=" + (type.empty() ? std::string("(unset)") : type) +
               " engine_model=\"" + model + "\"" +
               " engine_source=" + (source.empty() ? std::string("(unset: all producers)") : source) +
               " count=" + std::to_string(count) + " instances=" + instances +
               " helm_profile=" + helm_profile +
               " signalk_engine=" + (signalk_engine ? "1" : "0") +
               " moos_engine=" + (moos_engine ? "1" : "0") +
               " can_n2k_engine=" + (can_n2k_engine ? "1" : "0") +
               " can_dm1=" + (can_dm1 ? "1" : "0") +
               " can_j1939_engine=" + (can_j1939_engine ? "1 (engine SA " +
                                       std::to_string(j1939_engine_sa) + ")" : std::string("0"));
    }

private:
    static std::string Trim(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t");
        if (a == std::string::npos) return "";
        const size_t b = s.find_last_not_of(" \t");
        return s.substr(a, b - a + 1);
    }
    static std::string Lower(std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }
    // Tag values must not carry spaces or commas: "Mercury Diesel" -> mercury_diesel.
    static std::string Slug(const std::string& s) {
        std::string out;
        for (char ch : Lower(s)) {
            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') out += ch;
            else if (ch == ' ' || ch == ',' || ch == '=') out += '_';
        }
        return out;
    }
    static void Override(const std::string& v, bool* flag) {
        if (v == "1" || v == "true" || v == "yes") *flag = true;
        else if (v == "0" || v == "false" || v == "no") *flag = false;
    }
};

// MOOS ENGINE_* variable -> base name and engine instance. iCANRec names a
// second engine's variables with the instance as a suffix: ENGINE_SPEED is
// instance 0, ENGINE_SPEED_1 instance 1 (REC-7 in
// docs/requirements-usv-725.md). Any other name is instance 0 unchanged.
inline std::string EngineVarBase(const std::string& key, int* instance) {
    *instance = 0;
    const size_t us = key.rfind('_');
    if (us == std::string::npos || us + 1 >= key.size() || us + 3 < key.size())
        return key;
    for (size_t i = us + 1; i < key.size(); ++i)
        if (key[i] < '0' || key[i] > '9') return key;
    *instance = std::stoi(key.substr(us + 1));
    return key.substr(0, us);
}

}  // namespace usv
