// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Two projects in one engine, in C: the access-log report and Primes.
//
// Each project is a plugin of its own, and neither names the other. The engine runs both at the
// same time, on the same workers; when tasks of both wait for a worker, the report's go first,
// since they have the higher priority. Console prints the reports once all of them are in, in the
// order the projects were named, then ends the run.
//
// Build it with CMakeLists.txt or the Makefile in examples/c_cpp, then name the projects to run,
// or none for both:
//
//   projects_c [access-log] [primes]
//
// The Rust, C++ and Python versions in examples/ print the same.

#include "common.h"
#include "console.h"
#include "log_report.h"
#include "primes.h"

#include <stdio.h>
#include <string.h>

// The projects this program knows, by the names it's run with.
static const char* const PROJECTS[] = {LOG_REPORT_PROJECT, PRIMES_PROJECT};
#define PROJECT_COUNT (sizeof PROJECTS / sizeof PROJECTS[0])

// The project named `name`, as PROJECTS has it; NULL if there's none.
static const char* find_project(const char* name) {
    for (size_t i = 0; i < PROJECT_COUNT; i++) {
        if (strcmp(PROJECTS[i], name) == 0) {
            return PROJECTS[i];
        }
    }
    return NULL;
}

// Prints `count` names to `out`, with a comma between them.
static void print_names(FILE* out, const char* const* names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fprintf(out, "%s%s", i == 0 ? "" : ", ", names[i]);
    }
}

int main(int argc, char** argv) {
    // The projects named on the command line, each once, in the order given; all of them when
    // none is named. Console prints their reports in that order.
    Console console = {0};
    for (int i = 1; i < argc; i++) {
        const char* project = find_project(argv[i]);
        if (project == NULL) {
            fprintf(stderr, "unknown project: %s. The projects are: ", argv[i]);
            print_names(stderr, PROJECTS, PROJECT_COUNT);
            fprintf(stderr, "\n");
            return 2;
        }
        bool named = false;
        for (size_t j = 0; j < console.project_count; j++) {
            named = named || console.projects[j] == project;
        }
        if (!named) {
            console.projects[console.project_count++] = project;
        }
    }
    if (console.project_count == 0) {
        for (size_t i = 0; i < PROJECT_COUNT; i++) {
            console.projects[console.project_count++] = PROJECTS[i];
        }
    }

    // The files every example shares: the sample log, and Thryve's configuration.
    ThryveConfig config = thryve_config_from_file(EXAMPLES_DIR "/data/thryve.toml");
    if (config == NULL) {
        fprintf(stderr, "can't load the configuration: %s\n", last_error());
        return 1;
    }

    printf("Projects in this engine: ");
    print_names(stdout, console.projects, console.project_count);
    printf("\n");
    fflush(stdout);

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
    console.tx = tx;

    // Each project is a plugin of its own. Console prints their reports, in the order named. They
    // all live here for the whole run.
    LogReader reader = {EXAMPLES_DIR "/data/access.log", tx, ""};
    Primes primes = {tx, ""};
    bool added = true;
    for (size_t i = 0; i < console.project_count && added; i++) {
        ThryvePlugin plugin = strcmp(console.projects[i], LOG_REPORT_PROJECT) == 0
                                  ? log_reader_plugin(&reader)
                                  : primes_plugin(&primes);
        added = add_plugin(ctx, plugin);
    }
    if (!added || !add_observer(ctx, console_observer(&console))) {
        thryve_packet_handle_free(tx);
        thryve_ctx_free(ctx);
        thryve_config_free(config);
        return 1;
    }

    // The engine runs until Console's Fin task, or a project's FinErr, ends it.
    int exit_code = run_engine(ctx) ? 0 : 1;
    thryve_packet_handle_free(tx);
    thryve_config_free(config);
    return exit_code;
}
