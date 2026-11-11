// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// The access-log report on its own, in C: Thryve reads a web server's access log and reports on it.
//
// The report's steps are in log_report.c. Console, the observer that prints the report and ends
// the run, is in console.c.
//
// Build it with CMakeLists.txt or the Makefile in examples/c_cpp, then run it from anywhere; the
// log defaults to examples/data/access.log:
//
//   access_log_c [log]
//
// The Rust, C++ and Python versions in examples/ print the same report from the same log.

#include "common.h"
#include "console.h"
#include "log_report.h"

#include <stdio.h>

int main(int argc, char** argv) {
    // The files every example shares: the sample log, and Thryve's configuration.
    const char* log = argc > 1 ? argv[1] : EXAMPLES_DIR "/data/access.log";
    ThryveConfig config = thryve_config_from_file(EXAMPLES_DIR "/data/thryve.toml");
    if (config == NULL) {
        fprintf(stderr, "can't load the configuration: %s\n", last_error());
        return 1;
    }

    ThryveCtx ctx = thryve_ctx_create(config);
    if (ctx == NULL) {
        fprintf(stderr, "can't create the context: %s\n", last_error());
        thryve_config_free(config);
        return 1;
    }

    ThryvePacketHandle tx = thryve_ctx_get_packet_handle(ctx);
    if (tx == NULL) {
        fprintf(stderr, "can't get the packet handle: %s\n", last_error());
        thryve_ctx_free(ctx);
        thryve_config_free(config);
        return 1;
    }

    // LogReader starts the work and Console shows the result: neither knows the other. Both live
    // here for the whole run.
    LogReader reader = {log, tx, ""};
    Console console = {tx, {LOG_REPORT_PROJECT}, 1};
    if (!add_plugin(ctx, log_reader_plugin(&reader)) ||
        !add_observer(ctx, console_observer(&console))) {
        thryve_packet_handle_free(tx);
        thryve_ctx_free(ctx);
        thryve_config_free(config);
        return 1;
    }

    // The engine runs until Console's Fin task, or LogReader's FinErr, ends it.
    int exit_code = run_engine(ctx) ? 0 : 1;
    thryve_packet_handle_free(tx);
    thryve_config_free(config);
    return exit_code;
}
