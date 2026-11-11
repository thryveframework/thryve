// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Primes: counts the primes below 10,000,000. See primes.hpp.

#include "primes.hpp"

#include "common.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// The numbers below this one are counted.
constexpr std::uint32_t LIMIT = 10'000'000;
// How many ranges the numbers are cut into, each sieved by a Sieve task of its own.
constexpr std::size_t RANGES = 8;
// How many numbers each range holds.
constexpr std::uint32_t RANGE_SIZE = LIMIT / RANGES;
// The Sieve tasks' priority: low, so that tasks with a higher priority go first.
constexpr std::uint8_t PRIORITY = 1;
// Printed under the report's title, so that the numbers aren't taken for Thryve's own metrics.
constexpr const char* NOTE =
    "Note: The following numbers are calculations about the numbers below 10,000,000 and are not "
    "metrics related to Thryve itself.";

// ============================================================================
// Finding the primes
// ============================================================================

// What the sieve found in a run of numbers: one range, or several in a row.
struct Found {
    std::uint64_t primes = 0;
    std::uint64_t twin_pairs = 0;
    std::uint32_t largest_gap = 0;  // between two primes in a row; the first, when several are
    std::uint32_t gap_after = 0;    // the prime the largest gap comes after
    std::uint32_t first = 0;        // the first and the last prime, 0 while there's none: the
    std::uint32_t last = 0;         // twin pairs and gaps across runs are found with them

    // Notes the gap between `before` and `prime`, two primes in a row: a twin pair when it's 2,
    // and the largest gap when no gap before it is as large.
    void gap(std::uint32_t before, std::uint32_t prime) {
        std::uint32_t gap = prime - before;
        if (gap == 2) {
            ++twin_pairs;
        }
        if (gap > largest_gap) {
            largest_gap = gap;
            gap_after = before;
        }
    }

    // Counts `prime`, the next prime after the ones found so far, and the gap it ends.
    void add(std::uint32_t prime) {
        if (last != 0) {
            gap(last, prime);
        }
        ++primes;
        if (first == 0) {
            first = prime;
        }
        last = prime;
    }

    // Adds `run`, the next run of numbers, to what was found before it. A twin pair or a gap can
    // cross from one run into the next: from the last prime of one to the first of the next.
    void add_run(const Found& run) {
        if (last != 0 && run.first != 0) {
            gap(last, run.first);
        }
        primes += run.primes;
        twin_pairs += run.twin_pairs;
        if (run.largest_gap > largest_gap) {
            largest_gap = run.largest_gap;
            gap_after = run.gap_after;
        }
        if (first == 0) {
            first = run.first;
        }
        if (run.last != 0) {
            last = run.last;
        }
    }
};

// The whole part of the square root of `number`.
std::uint32_t square_root(std::uint32_t number) {
    std::uint32_t root = 0;
    while (static_cast<std::uint64_t>(root + 1) * (root + 1) <= number) {
        ++root;
    }
    return root;
}

// The primes up to `limit`, found with a plain sieve.
std::vector<std::uint32_t> primes_up_to(std::uint32_t limit) {
    std::vector<bool> crossed_out(static_cast<std::size_t>(limit) + 1);
    std::vector<std::uint32_t> primes;
    for (std::uint32_t number = 2; number <= limit; ++number) {
        if (crossed_out[number]) {
            continue;
        }
        primes.push_back(number);
        for (std::uint32_t multiple = number * number; multiple <= limit; multiple += number) {
            crossed_out[multiple] = true;
        }
    }
    return primes;
}

// The primes from `start` up to `end`, found by crossing out the multiples of the primes up to the
// range's square root. What's left is prime.
Found sieve_range(std::uint32_t start, std::uint32_t end) {
    std::vector<bool> crossed_out(end - start);
    for (std::uint32_t prime : primes_up_to(square_root(end - 1))) {
        // The prime's first multiple in the range, but never the prime itself.
        std::uint32_t first = std::max(prime * prime, (start + prime - 1) / prime * prime);
        for (std::uint32_t multiple = first; multiple < end; multiple += prime) {
            crossed_out[multiple - start] = true;
        }
    }

    Found found;
    // 0 and 1 aren't prime.
    for (std::uint32_t number = std::max(start, std::uint32_t{2}); number < end; ++number) {
        if (!crossed_out[number - start]) {
            found.add(number);
        }
    }
    return found;
}

// ============================================================================
// The steps, and how they're wired
// ============================================================================

// Sieve: finds the primes in one task's share of the ranges, `count` ranges from `first`.
ThryveOutcome sieve(std::size_t first, std::size_t count, void*) noexcept {
    return guarded([&] {
        Found found;
        for (std::size_t range = first; range < first + count; ++range) {
            auto start = static_cast<std::uint32_t>(range) * RANGE_SIZE;
            found.add_run(sieve_range(start, start + RANGE_SIZE));
        }
        return value(found);
    });
}

// Combine: adds up what the Sieve tasks found, in the ranges' order. If one failed, its error is
// passed on.
ThryveOutcome combine(ThryveHandoff sieved, void*) noexcept {
    return guarded([&] {
        if (thryve_handoff_is_error(sieved)) {
            return pass_error_on(sieved);
        }
        Found total;
        for (std::size_t i = 0; i < thryve_handoff_value_count(sieved); ++i) {
            total.add_run(read_value<Found>(sieved, i));
        }
        return value(total);
    });
}

// Write: writes the report from what Combine found, and sends it to Console. A failure is reported
// too, so that Console still gets a report from this project. Its user data is the engine's packet
// handle.
ThryveOutcome write_report(ThryveHandoff combined, void* tx) noexcept {
    return guarded([&] {
        std::ostringstream text;
        if (thryve_handoff_is_error(combined)) {
            text << "The primes report failed: " << handoff_error(combined) << "\n";
        } else {
            const Found& found = read_value<Found>(combined);
            text << "Primes report\n"
                 << NOTE << "\n"
                 << "  primes: " << found.primes << "\n"
                 << "  twin-prime pairs: " << found.twin_pairs << "\n"
                 << "  largest gap: " << found.largest_gap << ", after " << found.gap_after
                 << "\n";
        }
        send_report(static_cast<ThryvePacketHandle>(tx), PRIMES_PROJECT, text.str());
        return thryve_outcome_value(nullptr, nullptr);
    });
}

// Declares the whole count: Sieve, Combine and Write. Each step takes the handles of the steps
// before it, so the engine runs every step as soon as its input is ready. Every call uses up the
// handles it's given, even when it fails. Returns which step couldn't be declared, and why; empty
// once all of them are.
std::string declare_count(ThryvePacketHandle tx) {
    // Sieve: the numbers cut into 8 ranges, each sieved by a task of its own.
    ThryveAdvHandle sieved[RANGES];
    std::size_t sieved_count = 0;
    if (thryve_packet_handle_split_into(tx, RANGES, "Sieve", RANGES, PRIORITY, sieve, nullptr,
                                        nullptr, sieved, &sieved_count) != ThryveStatus_Ok) {
        return "Sieve: " + last_error();
    }

    // Combine: once all 8 are sieved, adds up what they found.
    ThryveAdvHandle combined =
        thryve_adv_handles_join(sieved, sieved_count, "Combine", combine, nullptr, nullptr);
    if (combined == nullptr) {
        return "Combine: " + last_error();
    }

    // Write: once Combine is done, sends the report to Console.
    ThryveAdvHandle written = thryve_adv_handle_then(combined, "Write", write_report, tx, nullptr);
    if (written == nullptr) {
        return "Write: " + last_error();
    }

    // Nothing follows Write: let its handle go.
    thryve_adv_handle_free(written);
    return {};
}

// The plugin's entry point, run once, on a worker, when the engine starts.
void start(ThryveTaskResult* result, void* core) noexcept {
    auto& primes = *static_cast<Primes*>(core);
    try {
        std::string failed = declare_count(primes.tx);
        if (!failed.empty()) {
            throw std::runtime_error("can't declare " + failed);
        }
        primes.message = "counting the primes below " + std::to_string(LIMIT);
        *result = thryve_task_result_new("Started", primes.message.c_str(), "Main");
    } catch (const std::exception& error) {
        primes.message = error.what();
        std::cerr << primes.message << "\n";
        *result = thryve_task_result_new("FinErr", primes.message.c_str(), "Error");
    }
}

// Primes has nothing to clean up when the engine stops.
int clean_up_primes(const void*) noexcept {
    return ThryveStatus_Ok;
}

}  // namespace

ThryvePlugin primes_plugin(Primes& primes) {
    return thryve_plugin_create("Primes", &primes, primes.tx, clean_up_primes, start);
}
