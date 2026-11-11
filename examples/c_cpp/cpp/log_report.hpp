// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// The access-log report: Thryve reads a web server's access log and reports on it.
//
// - LogReader, a plugin, reads the log once the engine starts, and cuts its lines into 8 pieces.
//   8 Parse tasks count one piece each, at the same time.
// - Merge waits for all 8 (a join) and adds their counts together.
// - Totals, StatusCodes, TopPages and Alerts each write one section of the report from Merge's
//   counts, which they share rather than copy (a fan-out).
// - Report waits for the four sections (another join) and sends the text to Console, an observer,
//   which prints it.
//
// LogReader and Console never name each other: the report reaches Console as a notification.

#ifndef EXAMPLES_LOG_REPORT_HPP
#define EXAMPLES_LOG_REPORT_HPP

#include "thryve_ffi.h"

#include <string>

// The project's name, as the programs know it.
inline constexpr const char* LOG_REPORT_PROJECT = "access-log";

// LogReader's state. It lives in main for the whole run.
struct LogReader {
    std::string log;        // the log's path
    ThryvePacketHandle tx;  // the engine's packet handle
    std::string message;    // the start's message, which has to outlive the entry point
};

// The LogReader plugin, for `reader`: it reads the log once the engine starts, and declares the
// report. NULL if it can't be made, with Thryve's last error saying why.
ThryvePlugin log_reader_plugin(LogReader& reader);

#endif
