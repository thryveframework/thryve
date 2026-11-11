// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Console: an observer that prints the projects' reports, then ends the run. See console.h.

#include "console.h"

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The task that ends the run: a Fin result stops the engine.
static void shut_down(ThryveTaskResult* result, void* user_data) {
    (void)user_data;
    *result = thryve_task_result_new("Fin", "the reports are out", "Main");
}

// Where `project` is among Console's projects; `project_count` if it isn't one of them.
static size_t place_of(const Console* console, const char* project) {
    size_t place = 0;
    while (place < console->project_count && strcmp(console->projects[place], project) != 0) {
        place++;
    }
    return place;
}

// The observer's job: its whole loop, run once, on its own thread. Every notification Console gets
// is a ProjectReport: only send_report sends to it.
static void print_reports(ThryveObserver observer) {
    const Console* console = thryve_observer_core(observer);

    // Each project's report, kept until all of them are in.
    char* reports[CONSOLE_MAX_PROJECTS] = {NULL};
    size_t missing = console->project_count;

    // thryve_observer_pop waits for the next notification, and returns NULL once the engine stops.
    ThryveNotif notif;
    while ((notif = thryve_observer_pop(observer)) != NULL) {
        ProjectReport* report = thryve_notification_core(notif);
        thryve_notification_free(notif);
        if (report == NULL) {
            continue;
        }
        size_t place = place_of(console, report->project);
        if (place == console->project_count) {
            free_project_report(report);
            continue;
        }
        if (reports[place] == NULL) {
            missing--;
        }
        free(reports[place]);
        reports[place] = report->text;
        free(report);
        if (missing > 0) {
            continue;
        }

        // Every report is in: they're printed a blank line apart.
        for (size_t i = 0; i < console->project_count; i++) {
            if (i > 0) {
                fputc('\n', stdout);
            }
            fputs(reports[i], stdout);
            free(reports[i]);
            reports[i] = NULL;
        }
        fflush(stdout);
        missing = console->project_count;

        // The reports are out: a task with a Fin result ends the run.
        ThryveTask fin = thryve_task_create("Shutdown", 0, shut_down, NULL, NULL);
        if (fin != NULL) {
            thryve_packet_handle_send_task(console->tx, fin);
        }
    }

    // The engine stopped before every report was in: the ones that came are let go.
    for (size_t i = 0; i < console->project_count; i++) {
        free(reports[i]);
    }
}

// Console has nothing to clean up when the engine stops.
static void clean_up_console(const void* core) {
    (void)core;
}

ThryveObserver console_observer(Console* console) {
    return thryve_observer_create("Console", console, console->tx, clean_up_console,
                                  print_reports);
}
