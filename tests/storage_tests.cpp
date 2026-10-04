// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "storage.h"
#include "parameter_json.h"
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <windows.h>

namespace hikari {
void runStorageTests() {
    auto root = std::filesystem::temp_directory_path() / ("HikariSoundEngine-selftest-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Storage storage(root);
    auto check = [](bool v) { if (!v) throw std::runtime_error("Storage self-test failed"); };
    try {
        check(storage.read("state.json").isNull());
        check(!std::filesystem::exists(root));
        Parameters p; p.eq.mode = "parametric"; p.eq.filters.push_back({true, "PK", 1000, 3, 1.4}); p.effects.clarity = 3;
        auto params = parametersJson(p);
        auto state = Json::object({{"schema", 1}, {"applied", params}, {"cleanExit", false}});
        storage.writeState(state);
        check(storage.read("state.json").stringify() == state.stringify());
        check(parametersJson(parseParameters(params)).stringify() == params.stringify());
        // A held compare-with-original must not survive a restart, including state written by 1.0.0.
        auto held = p; held.bypass = true;
        check(persistedParametersJson(held).stringify() == params.stringify());
        check(parametersJson(held).at("bypass").asBool() && !restoredParameters(parametersJson(held)).bypass);
        check(parametersJson(restoredParameters(parametersJson(held))).stringify() == params.stringify());
        bool rejected = false;
        try { auto invalid = params; invalid["bypass"] = "false"; parseParameters(invalid); } catch (const JsonError&) { rejected = true; }
        check(rejected);
        Parameters graph; graph.eq.mode = "graphic"; graph.eq.points = {{20, -12}, {1000, 3}, {20000, 12}};
        const auto graphJson = parametersJson(graph);
        check(parametersJson(parseParameters(graphJson)).stringify() == graphJson.stringify());
        check(graphJson.at("eq").contains("points") && !graphJson.at("eq").contains("bands"));
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            auto bad = graphJson;
            if (scenario == 0) bad["eq"]["points"] = Json::Array{};
            if (scenario == 1) bad["eq"]["points"] = Json::array({Json::object({{"freq", 1000}, {"gain", 0}}), Json::object({{"freq", 20}, {"gain", 0}})});
            if (scenario == 2) bad["eq"]["points"] = Json::array({Json::object({{"freq", 1000}, {"gain", "3"}})});
            if (scenario == 3) bad["eq"]["bands"] = Json::Array{};
            rejected = false; try { parseParameters(bad); } catch (const JsonError&) { rejected = true; } check(rejected);
        }
        state["cleanExit"] = true; storage.writeState(state); check(storage.read("state.json").at("cleanExit").asBool());
        check(!std::filesystem::exists(root / "state.json.tmp"));
        std::ofstream(root / "state.json", std::ios::binary) << "invalid";
        rejected = false; try { storage.read("state.json"); } catch (...) { rejected = true; } check(rejected);
        std::filesystem::remove_all(root);
    } catch (...) { std::filesystem::remove_all(root); throw; }
}
}
