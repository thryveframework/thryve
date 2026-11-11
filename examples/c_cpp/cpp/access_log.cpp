// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// The access-log report on its own, in C++: Thryve reads a web server's access log and reports on
// it.
//
// The report's steps are in log_report.cpp. Console, the observer that prints the report and ends
// the run, is in console.cpp.
//
// Build it with CMakeLists.txt or the Makefile in examples/c_cpp, then run it from anywhere; the
// log defaults to examples/data/access.log:
//
//   access_log_cpp [log]
//
// The Rust, C and Python versions in examples/ print the same report from the same log.

#include "common.hpp"
#include "console.hpp"
#include "log_report.hpp"

#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    // The files every example shares: the sample log, and Thryve's configuration. Each handle is
    // freed when main returns, with the function that goes with it.
    const std::string log = argc > 1 ? argv[1] : EXAMPLES_DIR "/data/access.log";
    std::unique_ptr<ConfigHandle, decltype(&thryve_config_free)> config(
        thryve_config_from_file(EXAMPLES_DIR "/data/thryve.toml"), &thryve_config_free);
    if (config == nullptr) {
        std::cerr << "can't load the configuration: " << last_error() << "\n";
        return 1;
    }

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

    // LogReader starts the work and Console shows the result: neither knows the other. Both live
    // here for the whole run.
    LogReader reader{log, tx.get(), {}};
    Console console{tx.get(), {LOG_REPORT_PROJECT}};
    if (!add_plugin(ctx.get(), log_reader_plugin(reader)) ||
        !add_observer(ctx.get(), console_observer(console))) {
        return 1;
    }

    // The engine runs until Console's Fin task, or LogReader's FinErr, ends it. Running it uses up
    // the context.
    return run_engine(ctx.release()) ? 0 : 1;
}
