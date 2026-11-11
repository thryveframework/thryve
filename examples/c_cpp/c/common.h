// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the C examples have in common: where the shared files are, Thryve's last error, a string
// that grows as it's written, the outcomes every job needs, the notification that carries a
// project's report to Console, and running the engine.

#ifndef EXAMPLES_COMMON_H
#define EXAMPLES_COMMON_H

#include "thryve.h"

#include <stdbool.h>
#include <stddef.h>

// The examples folder, which holds the files every example shares: data/access.log and
// data/thryve.toml. CMakeLists.txt and the Makefile set it. Built by hand without it, the programs
// find them only when they're run from examples/c_cpp.
#ifndef EXAMPLES_DIR
#define EXAMPLES_DIR ".."
#endif

// Why the latest call to Thryve failed.
const char* last_error(void);

// A string being written. Once a write runs out of memory, it stays failed.
typedef struct {
    char* data;
    size_t length;
    size_t room;
    bool failed;
} Text;

// Adds to the text, printf-style.
void text_add(Text* text, const char* format, ...);

// The finished text, for the caller to free; NULL if writing it ran out of memory.
char* text_finish(Text* text);

// Passes on the error a task was handed, as the task's own.
ThryveOutcome pass_error_on(ThryveHandoff handoff);

// The error of a job that ran out of memory.
ThryveOutcome out_of_memory(void);

// The message of the error a task was handed.
const char* handoff_error(ThryveHandoff handoff);

// A project's report, on its way to Console as a notification. It's all the projects and Console
// share: neither names the other.
typedef struct {
    const char* project;  // the project's name, as the programs know it
    char* text;           // the report
} ProjectReport;

// Sends `project`'s report to Console, as a notification, and takes `text`, which came from
// malloc. Console frees the report once it has printed it.
void send_report(ThryvePacketHandle tx, const char* project, char* text);

// Frees a report and its text.
void free_project_report(ProjectReport* report);

// Adds a plugin to the context, which owns it from then on. False if it couldn't be made or added,
// with the reason on stderr; one that can't be added is freed.
bool add_plugin(ThryveCtx ctx, ThryvePlugin plugin);

// Adds an observer to the context, the same way.
bool add_observer(ThryveCtx ctx, ThryveObserver observer);

// Starts the engine, which uses up the context, and waits for it to end. False if it couldn't
// run, with the reason on stderr.
bool run_engine(ThryveCtx ctx);

#endif
