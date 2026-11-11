// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the C++ examples have in common. See common.hpp.

#include "common.hpp"

#include <iostream>

std::string last_error() {
    const char* error = thryve_last_error();
    return error != nullptr && *error != '\0' ? error : "unknown error";
}

ThryveOutcome pass_error_on(ThryveHandoff handoff) {
    return thryve_outcome_error(thryve_handoff_error_code(handoff),
                                thryve_handoff_error_message(handoff));
}

std::string handoff_error(ThryveHandoff handoff) {
    const char* error = thryve_handoff_error_message(handoff);
    return error != nullptr ? error : "unknown error";
}

void send_report(ThryvePacketHandle tx, const char* project, std::string text) {
    // Thryve never frees a notification's data: Console deletes the report once it has printed
    // it, and one that isn't sent is deleted here. Sending fails only once the engine is gone, and
    // then there's nobody left to tell.
    auto* report = new ProjectReport{project, std::move(text)};
    ThryveNotif notif = thryve_notification_create("Report", "Console", report);
    if (notif == nullptr || thryve_packet_handle_send_notification(tx, notif) != ThryveStatus_Ok) {
        delete report;
    }
}

bool add_plugin(ThryveCtx ctx, ThryvePlugin plugin) {
    if (plugin == nullptr || thryve_ctx_add_plugin(ctx, plugin) != ThryveStatus_Ok) {
        // The reason first: the next call to Thryve replaces it.
        std::cerr << "can't add a plugin: " << last_error() << "\n";
        if (plugin != nullptr) {
            thryve_plugin_free(plugin);
        }
        return false;
    }
    return true;
}

bool add_observer(ThryveCtx ctx, ThryveObserver observer) {
    if (observer == nullptr || thryve_ctx_add_observer(ctx, observer) != ThryveStatus_Ok) {
        // The reason first: the next call to Thryve replaces it.
        std::cerr << "can't add an observer: " << last_error() << "\n";
        if (observer != nullptr) {
            thryve_observer_free(observer);
        }
        return false;
    }
    return true;
}

bool run_engine(ThryveCtx ctx) {
    // Freeing the run waits for the engine to end.
    ThryveRunHandle run = thryve_run_interactive_start(ctx);
    if (run == nullptr || thryve_run_interactive_free(run) != ThryveStatus_Ok) {
        std::cerr << "the run failed: " << last_error() << "\n";
        return false;
    }
    return true;
}
