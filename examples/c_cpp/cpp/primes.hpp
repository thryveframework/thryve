// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Primes: counts the primes below 10,000,000, the twin-prime pairs among them, and the largest gap
// between two primes in a row.
//
// - Primes, a plugin, cuts the numbers into 8 ranges once the engine starts. 8 Sieve tasks find
//   the primes in one range each, at the same time.
// - Combine waits for all 8 (a join) and adds up what they found, in the ranges' order.
// - Write follows Combine (a `then` step): it writes the report and sends it to Console, an
//   observer, which prints it.
//
// The Sieve tasks run at a low priority, 1. When other tasks wait for a worker too, those with a
// higher priority go first; Thryve raises the priority of a task that waits, so the Sieve tasks
// still get their turn.
//
// Anyone can check the numbers: 664,579 primes (OEIS A006880), 58,980 twin-prime pairs (OEIS
// A007508), and a largest gap of 154, after 4,652,353 (Wikipedia, "Prime gap").

#ifndef EXAMPLES_PRIMES_HPP
#define EXAMPLES_PRIMES_HPP

#include "thryve_ffi.h"

#include <string>

// The project's name, as the programs know it.
inline constexpr const char* PRIMES_PROJECT = "primes";

// Primes' state. It lives in main for the whole run.
struct Primes {
    ThryvePacketHandle tx;  // the engine's packet handle
    std::string message;    // the start's message, which has to outlive the entry point
};

// The Primes plugin, for `primes`: it counts the primes once the engine starts. NULL if it can't
// be made, with Thryve's last error saying why.
ThryvePlugin primes_plugin(Primes& primes);

#endif
