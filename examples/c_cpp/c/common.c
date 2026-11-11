// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the C examples have in common. See common.h.

#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

const char* last_error(void) {
    const char* error = thryve_last_error();
    return error != NULL && error[0] != '\0' ? error : "unknown error";
}

void text_add(Text* text, const char* format, ...) {
    if (text->failed) {
        return;
    }

    va_list args;
    va_start(args, format);
    int needed = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (needed < 0) {
        text->failed = true;
        return;
    }

    // Room for what's there, what's added, and the closing '\0'.
    size_t length = text->length + (size_t)needed;
    if (length + 1 > text->room) {
        size_t room = text->room == 0 ? 256 : text->room;
        while (room < length + 1) {
            room *= 2;
        }
        char* data = realloc(text->data, room);
        if (data == NULL) {
            text->failed = true;
            return;
        }
        text->data = data;
        text->room = room;
    }

    va_start(args, format);
    vsnprintf(text->data + text->length, text->room - text->length, format, args);
    va_end(args);
    text->length = length;
}

char* text_finish(Text* text) {
    if (text->failed) {
        free(text->data);
        return NULL;
    }
    return text->data != NULL ? text->data : calloc(1, 1);
}

ThryveOutcome pass_error_on(ThryveHandoff handoff) {
    return thryve_outcome_error(thryve_handoff_error_code(handoff),
                                thryve_handoff_error_message(handoff));
}

ThryveOutcome out_of_memory(void) {
    return thryve_outcome_error(1, "out of memory");
}

const char* handoff_error(ThryveHandoff handoff) {
    const char* error = thryve_handoff_error_message(handoff);
    return error != NULL ? error : "unknown error";
}

void send_report(ThryvePacketHandle tx, const char* project, char* text) {
    // Thryve never frees a notification's data: Console frees the report once it has printed it,
    // and one that isn't sent is freed here. Sending fails only once the engine is gone, and then
    // there's nobody left to tell.
    ProjectReport* report = malloc(sizeof *report);
    if (report == NULL) {
        free(text);
        return;
    }
    *report = (ProjectReport){project, text};

    ThryveNotif notif = thryve_notification_create("Report", "Console", report);
    if (notif == NULL || thryve_packet_handle_send_notification(tx, notif) != ThryveStatus_Ok) {
        free_project_report(report);
    }
}

void free_project_report(ProjectReport* report) {
    free(report->text);
    free(report);
}

bool add_plugin(ThryveCtx ctx, ThryvePlugin plugin) {
    if (plugin == NULL || thryve_ctx_add_plugin(ctx, plugin) != ThryveStatus_Ok) {
        // The reason first: the next call to Thryve replaces it.
        fprintf(stderr, "can't add a plugin: %s\n", last_error());
        if (plugin != NULL) {
            thryve_plugin_free(plugin);
        }
        return false;
    }
    return true;
}

bool add_observer(ThryveCtx ctx, ThryveObserver observer) {
    if (observer == NULL || thryve_ctx_add_observer(ctx, observer) != ThryveStatus_Ok) {
        // The reason first: the next call to Thryve replaces it.
        fprintf(stderr, "can't add an observer: %s\n", last_error());
        if (observer != NULL) {
            thryve_observer_free(observer);
        }
        return false;
    }
    return true;
}

bool run_engine(ThryveCtx ctx) {
    // Freeing the run waits for the engine to end.
    ThryveRunHandle run = thryve_run_interactive_start(ctx);
    if (run == NULL || thryve_run_interactive_free(run) != ThryveStatus_Ok) {
        fprintf(stderr, "the run failed: %s\n", last_error());
        return false;
    }
    return true;
}
