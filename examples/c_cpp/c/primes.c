// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Primes: counts the primes below 10,000,000. See primes.h.

#include "primes.h"

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// The numbers below this one are counted.
#define LIMIT 10000000u
// How many ranges the numbers are cut into, each sieved by a Sieve task of its own.
#define RANGES 8
// How many numbers each range holds.
#define RANGE_SIZE (LIMIT / RANGES)
// The Sieve tasks' priority: low, so that tasks with a higher priority go first.
#define PRIORITY 1
// Printed under the report's title, so that the numbers aren't taken for Thryve's own metrics.
#define NOTE                                                                                  \
    "Note: The following numbers are calculations about the numbers below 10,000,000 and are " \
    "not metrics related to Thryve itself."

// ============================================================================
// Finding the primes
// ============================================================================

// What the sieve found in a run of numbers: one range, or several in a row.
typedef struct {
    uint64_t primes;
    uint64_t twin_pairs;
    uint32_t largest_gap;  // between two primes in a row; the first one, when several are as large
    uint32_t gap_after;    // the prime the largest gap comes after
    uint32_t first;        // the first and the last prime, 0 while there's none: the twin pairs
    uint32_t last;         // and gaps across runs are found with them
} Found;

// Notes the gap between `before` and `prime`, two primes in a row: a twin pair when it's 2, and the
// largest gap when no gap before it is as large.
static void note_gap(Found* found, uint32_t before, uint32_t prime) {
    uint32_t gap = prime - before;
    if (gap == 2) {
        found->twin_pairs++;
    }
    if (gap > found->largest_gap) {
        found->largest_gap = gap;
        found->gap_after = before;
    }
}

// Counts `prime`, the next prime after the ones found so far, and the gap it ends.
static void add_prime(Found* found, uint32_t prime) {
    if (found->last != 0) {
        note_gap(found, found->last, prime);
    }
    found->primes++;
    if (found->first == 0) {
        found->first = prime;
    }
    found->last = prime;
}

// The whole part of the square root of `number`.
static uint32_t square_root(uint32_t number) {
    uint32_t root = 0;
    while ((uint64_t)(root + 1) * (root + 1) <= number) {
        root++;
    }
    return root;
}

// The primes up to `limit`, found with a plain sieve: `*count` of them, for the caller to free.
// NULL when out of memory.
static uint32_t* primes_up_to(uint32_t limit, size_t* count) {
    bool* crossed_out = calloc((size_t)limit + 1, sizeof *crossed_out);
    uint32_t* primes = malloc(((size_t)limit + 1) * sizeof *primes);
    if (crossed_out == NULL || primes == NULL) {
        free(crossed_out);
        free(primes);
        return NULL;
    }

    *count = 0;
    for (uint32_t number = 2; number <= limit; number++) {
        if (crossed_out[number]) {
            continue;
        }
        primes[(*count)++] = number;
        for (uint32_t multiple = number * number; multiple <= limit; multiple += number) {
            crossed_out[multiple] = true;
        }
    }
    free(crossed_out);
    return primes;
}

// The primes from `start` up to `end`, added to `found`. They're found by crossing out the
// multiples of the primes up to the range's square root: what's left is prime. False when out of
// memory.
static bool sieve_range(uint32_t start, uint32_t end, Found* found) {
    size_t prime_count = 0;
    uint32_t* primes = primes_up_to(square_root(end - 1), &prime_count);
    bool* crossed_out = calloc(end - start, sizeof *crossed_out);
    if (primes == NULL || crossed_out == NULL) {
        free(primes);
        free(crossed_out);
        return false;
    }

    for (size_t i = 0; i < prime_count; i++) {
        // The prime's first multiple in the range, but never the prime itself.
        uint32_t prime = primes[i];
        uint32_t first = (start + prime - 1) / prime * prime;
        if (first < prime * prime) {
            first = prime * prime;
        }
        for (uint32_t multiple = first; multiple < end; multiple += prime) {
            crossed_out[multiple - start] = true;
        }
    }

    // 0 and 1 aren't prime.
    for (uint32_t number = start < 2 ? 2 : start; number < end; number++) {
        if (!crossed_out[number - start]) {
            add_prime(found, number);
        }
    }
    free(primes);
    free(crossed_out);
    return true;
}

// Adds `run`, the next run of numbers, to what was found before it, in `total`. A twin pair or a
// gap can cross from one run into the next: from the last prime of one to the first of the next.
static void add_run(Found* total, const Found* run) {
    if (total->last != 0 && run->first != 0) {
        note_gap(total, total->last, run->first);
    }
    total->primes += run->primes;
    total->twin_pairs += run->twin_pairs;
    if (run->largest_gap > total->largest_gap) {
        total->largest_gap = run->largest_gap;
        total->gap_after = run->gap_after;
    }
    if (total->first == 0) {
        total->first = run->first;
    }
    if (run->last != 0) {
        total->last = run->last;
    }
}

// ============================================================================
// The steps, and how they're wired
// ============================================================================

// Sieve: finds the primes in one task's share of the ranges, `count` ranges from `first`.
static ThryveOutcome sieve(size_t first, size_t count, void* user_data) {
    (void)user_data;
    Found* found = calloc(1, sizeof *found);
    if (found == NULL) {
        return out_of_memory();
    }
    for (size_t range = first; range < first + count; range++) {
        Found run = {0};
        uint32_t start = (uint32_t)range * RANGE_SIZE;
        if (!sieve_range(start, start + RANGE_SIZE, &run)) {
            free(found);
            return out_of_memory();
        }
        add_run(found, &run);
    }
    return thryve_outcome_value(found, free);
}

// Combine: adds up what the Sieve tasks found, in the ranges' order. If one failed, its error is
// passed on.
static ThryveOutcome combine(ThryveHandoff sieved, void* user_data) {
    (void)user_data;
    if (thryve_handoff_is_error(sieved)) {
        return pass_error_on(sieved);
    }

    Found* total = calloc(1, sizeof *total);
    if (total == NULL) {
        return out_of_memory();
    }
    size_t count = thryve_handoff_value_count(sieved);
    for (size_t i = 0; i < count; i++) {
        add_run(total, thryve_handoff_value(sieved, i));
    }
    return thryve_outcome_value(total, free);
}

// Write: writes the report from what Combine found, and sends it to Console. A failure is reported
// too, so that Console still gets a report from this project. Its user data is the engine's packet
// handle.
static ThryveOutcome write_report(ThryveHandoff combined, void* tx) {
    Text text = {0};
    if (thryve_handoff_is_error(combined)) {
        text_add(&text, "The primes report failed: %s\n", handoff_error(combined));
    } else {
        const Found* found = thryve_handoff_value(combined, 0);
        text_add(&text, "Primes report\n" NOTE "\n");
        text_add(&text, "  primes: %llu\n", (unsigned long long)found->primes);
        text_add(&text, "  twin-prime pairs: %llu\n", (unsigned long long)found->twin_pairs);
        text_add(&text, "  largest gap: %lu, after %lu\n", (unsigned long)found->largest_gap,
                 (unsigned long)found->gap_after);
    }
    char* report = text_finish(&text);
    if (report == NULL) {
        return out_of_memory();
    }
    send_report(tx, PRIMES_PROJECT, report);
    return thryve_outcome_value(NULL, NULL);
}

// Notes why a step couldn't be declared, before another call to Thryve replaces the error.
static bool declare_failed(Primes* primes, const char* step) {
    snprintf(primes->message, sizeof primes->message, "can't declare %s: %s", step, last_error());
    return false;
}

// Declares the whole count: Sieve, Combine and Write. Each step takes the handles of the steps
// before it, so the engine runs every step as soon as its input is ready. Every call uses up the
// handles it's given, even when it fails.
static bool declare_count(Primes* primes) {
    // Sieve: the numbers cut into 8 ranges, each sieved by a task of its own.
    ThryveAdvHandle sieved[RANGES];
    size_t sieved_count = 0;
    if (thryve_packet_handle_split_into(primes->tx, RANGES, "Sieve", RANGES, PRIORITY, sieve, NULL,
                                        NULL, sieved, &sieved_count) != ThryveStatus_Ok) {
        return declare_failed(primes, "Sieve");
    }

    // Combine: once all 8 are sieved, adds up what they found.
    ThryveAdvHandle combined =
        thryve_adv_handles_join(sieved, sieved_count, "Combine", combine, NULL, NULL);
    if (combined == NULL) {
        return declare_failed(primes, "Combine");
    }

    // Write: once Combine is done, sends the report to Console.
    ThryveAdvHandle written =
        thryve_adv_handle_then(combined, "Write", write_report, primes->tx, NULL);
    if (written == NULL) {
        return declare_failed(primes, "Write");
    }

    // Nothing follows Write: let its handle go.
    thryve_adv_handle_free(written);
    return true;
}

// The plugin's entry point, run once, on a worker, when the engine starts.
static void start(ThryveTaskResult* result, void* core) {
    Primes* primes = core;
    if (!declare_count(primes)) {
        fprintf(stderr, "%s\n", primes->message);
        *result = thryve_task_result_new("FinErr", primes->message, "Error");
        return;
    }
    snprintf(primes->message, sizeof primes->message, "counting the primes below %lu",
             (unsigned long)LIMIT);
    *result = thryve_task_result_new("Started", primes->message, "Main");
}

// Primes has nothing to clean up when the engine stops.
static int clean_up_primes(const void* core) {
    (void)core;
    return ThryveStatus_Ok;
}

ThryvePlugin primes_plugin(Primes* primes) {
    return thryve_plugin_create("Primes", primes, primes->tx, clean_up_primes, start);
}
