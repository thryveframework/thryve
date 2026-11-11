// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the C++ examples have in common: where the shared files are, Thryve's last error, handing
// C++ values to Thryve and back, the notification that carries a project's report to Console, and
// running the engine.

#ifndef EXAMPLES_COMMON_HPP
#define EXAMPLES_COMMON_HPP

#include "thryve_ffi.h"  // thryve.h, in an extern "C" block

#include <cstddef>
#include <exception>
#include <string>
#include <utility>

// The examples folder, which holds the files every example shares: data/access.log and
// data/thryve.toml. CMakeLists.txt and the Makefile set it. Built by hand without it, the programs
// find them only when they're run from examples/c_cpp.
#ifndef EXAMPLES_DIR
#define EXAMPLES_DIR ".."
#endif

// Why the latest call to Thryve failed.
std::string last_error();

// A job's outcome: its value, which Thryve deletes once nobody can take it any more.
template <typename T>
ThryveOutcome value(T held) {
    return thryve_outcome_value(new T(std::move(held)),
                                [](void* kept) { delete static_cast<T*>(kept); });
}

// Reads a value in place. Every follower of a fan-out reads the same one, so none may change it.
template <typename T>
const T& read_value(ThryveHandoff handoff, std::size_t index = 0) {
    return *static_cast<const T*>(thryve_handoff_value(handoff, index));
}

// Passes on the error a task was handed, as the task's own.
ThryveOutcome pass_error_on(ThryveHandoff handoff);

// The message of the error a task was handed.
std::string handoff_error(ThryveHandoff handoff);

// Runs a job's body. Nothing may throw back into Thryve: an exception becomes the task's error.
template <typename Body>
ThryveOutcome guarded(Body body) noexcept {
    try {
        return body();
    } catch (const std::exception& error) {
        return thryve_outcome_error(1, error.what());
    } catch (...) {
        return thryve_outcome_error(1, "unknown error");
    }
}

// A project's report, on its way to Console as a notification. It's all the projects and Console
// share: neither names the other.
struct ProjectReport {
    std::string project;  // the project's name, as the programs know it
    std::string text;     // the report
};

// Sends `project`'s report to Console, as a notification. Console deletes it once it has printed
// it.
void send_report(ThryvePacketHandle tx, const char* project, std::string text);

// Adds a plugin to the context, which owns it from then on. False if it couldn't be made or added,
// with the reason on stderr; one that can't be added is freed.
bool add_plugin(ThryveCtx ctx, ThryvePlugin plugin);

// Adds an observer to the context, the same way.
bool add_observer(ThryveCtx ctx, ThryveObserver observer);

// Starts the engine, which uses up the context, and waits for it to end. False if it couldn't
// run, with the reason on stderr.
bool run_engine(ThryveCtx ctx);

#endif
