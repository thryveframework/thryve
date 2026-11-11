// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Console: an observer that prints the projects' reports, then ends the run.
//
// Each project sends its report as a notification. Console keeps the reports until every project's
// is in, and prints them in the order it was given the projects: the output is the same on every
// run, whichever project finishes first.

#ifndef EXAMPLES_CONSOLE_HPP
#define EXAMPLES_CONSOLE_HPP

#include "thryve_ffi.h"

#include <string>
#include <vector>

// Console's state. It lives in main for the whole run.
struct Console {
    ThryvePacketHandle tx;              // the engine's packet handle
    std::vector<std::string> projects;  // in the order their reports are printed
};

// The Console observer, for `console`. NULL if it can't be made, with Thryve's last error saying
// why.
ThryveObserver console_observer(Console& console);

#endif
