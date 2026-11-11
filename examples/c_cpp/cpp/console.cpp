// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Console: an observer that prints the projects' reports, then ends the run. See console.hpp.

#include "console.hpp"

#include "common.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

// The task that ends the run: a Fin result stops the engine.
void shut_down(ThryveTaskResult* result, void*) noexcept {
    *result = thryve_task_result_new("Fin", "the reports are out", "Main");
}

// The observer's job: its whole loop, run once, on its own thread. Every notification Console gets
// is a ProjectReport: only send_report sends to it.
void print_reports(ThryveObserver observer) noexcept {
    const auto& console = *static_cast<const Console*>(thryve_observer_core(observer));

    // Each project's report, kept until all of them are in.
    std::vector<std::optional<std::string>> reports(console.projects.size());

    // thryve_observer_pop waits for the next notification, and returns NULL once the engine stops.
    while (ThryveNotif notif = thryve_observer_pop(observer)) {
        std::unique_ptr<ProjectReport> report(
            static_cast<ProjectReport*>(thryve_notification_core(notif)));
        thryve_notification_free(notif);
        if (report == nullptr) {
            continue;
        }
        auto place = std::find(console.projects.begin(), console.projects.end(), report->project);
        if (place == console.projects.end()) {
            continue;
        }
        reports[static_cast<std::size_t>(place - console.projects.begin())] =
            std::move(report->text);
        if (std::any_of(reports.begin(), reports.end(), [](const auto& kept) { return !kept; })) {
            continue;
        }

        // Every report is in: they're printed a blank line apart.
        for (std::size_t i = 0; i < reports.size(); ++i) {
            std::cout << (i == 0 ? "" : "\n") << *reports[i];
            reports[i].reset();
        }
        std::cout << std::flush;

        // The reports are out: a task with a Fin result ends the run.
        ThryveTask fin = thryve_task_create("Shutdown", 0, shut_down, nullptr, nullptr);
        if (fin != nullptr) {
            thryve_packet_handle_send_task(console.tx, fin);
        }
    }
}

// Console has nothing to clean up when the engine stops.
void clean_up_console(const void*) noexcept {}

}  // namespace

ThryveObserver console_observer(Console& console) {
    return thryve_observer_create("Console", &console, console.tx, clean_up_console,
                                  print_reports);
}
