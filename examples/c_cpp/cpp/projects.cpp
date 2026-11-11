// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Two projects in one engine, in C++: the access-log report and Primes.
//
// Each project is a plugin of its own, and neither names the other. The engine runs both at the
// same time, on the same workers; when tasks of both wait for a worker, the report's go first,
// since they have the higher priority. Console prints the reports once all of them are in, in the
// order the projects were named, then ends the run.
//
// Build it with CMakeLists.txt or the Makefile in examples/c_cpp, then name the projects to run,
// or none for both:
//
//   projects_cpp [access-log] [primes]
//
// The Rust, C and Python versions in examples/ print the same.

#include "common.hpp"
#include "console.hpp"
#include "log_report.hpp"
#include "primes.hpp"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

// The projects this program knows, by the names it's run with.
const std::vector<std::string> PROJECTS = {LOG_REPORT_PROJECT, PRIMES_PROJECT};

// The names, with a comma between them.
std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    // The projects named on the command line, each once, in the order given; all of them when
    // none is named.
    std::vector<std::string> chosen;
    for (int i = 1; i < argc; ++i) {
        std::string name = argv[i];
        if (std::find(PROJECTS.begin(), PROJECTS.end(), name) == PROJECTS.end()) {
            std::cerr << "unknown project: " << name << ". The projects are: " << joined(PROJECTS)
                      << "\n";
            return 2;
        }
        if (std::find(chosen.begin(), chosen.end(), name) == chosen.end()) {
            chosen.push_back(name);
        }
    }
    if (chosen.empty()) {
        chosen = PROJECTS;
    }

    // The files every example shares: the sample log, and Thryve's configuration. Each handle is
    // freed when main returns, with the function that goes with it.
    std::unique_ptr<ConfigHandle, decltype(&thryve_config_free)> config(
        thryve_config_from_file(EXAMPLES_DIR "/data/thryve.toml"), &thryve_config_free);
    if (config == nullptr) {
        std::cerr << "can't load the configuration: " << last_error() << "\n";
        return 1;
    }

    std::cout << "Projects in this engine: " << joined(chosen) << std::endl;

    std::unique_ptr<CtxHandle, decltype(&thryve_ctx_free)> ctx(thryve_ctx_create(config.get()),
                                                               &thryve_ctx_free);
    if (ctx == nullptr) {
        std::cerr << "can't create the context: " << last_error() << "\n";
        return 1;
    }

    std::unique_ptr<PacketHandleHandle, decltype(&thryve_packet_handle_free)> tx(
        thryve_ctx_get_packet_handle(ctx.get()), &thryve_packet_handle_free);
    if (tx == nullptr) {
        std::cerr << "can't get the packet handle: " << last_error() << "\n";
        return 1;
    }

    // Each project is a plugin of its own. Console prints their reports, in the order named. They
    // all live here for the whole run.
    LogReader reader{EXAMPLES_DIR "/data/access.log", tx.get(), {}};
    Primes primes{tx.get(), {}};
    Console console{tx.get(), chosen};
    for (const std::string& project : chosen) {
        ThryvePlugin plugin =
            project == LOG_REPORT_PROJECT ? log_reader_plugin(reader) : primes_plugin(primes);
        if (!add_plugin(ctx.get(), plugin)) {
            return 1;
        }
    }
    if (!add_observer(ctx.get(), console_observer(console))) {
        return 1;
    }

    // The engine runs until Console's Fin task, or a project's FinErr, ends it. Running it uses up
    // the context.
    return run_engine(ctx.release()) ? 0 : 1;
}
