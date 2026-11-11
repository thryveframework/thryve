# Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
#
# This file is part of Thryve, dual-licensed under the GNU Affero General
# Public License v3.0 (or later) and a separate Commercial License.
# See LICENSE and COMMERCIAL.md in the repository root, or visit
# https://thryve-framework.com for licensing options.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Primes: counts the primes below 10,000,000, the twin-prime pairs among them, and the largest gap
between two primes in a row.

- Primes, a plugin, cuts the numbers into 8 ranges once the engine starts. 8 Sieve tasks find the
  primes in one range each, at the same time.
- Combine waits for all 8 (a join) and adds up what they found, in the ranges' order.
- Write follows Combine (a `then` step): it writes the report and sends it to Console, an observer,
  which prints it.

The Sieve tasks run at a low priority, 1. When other tasks wait for a worker too, those with a
higher priority go first; Thryve raises the priority of a task that waits, so the Sieve tasks still
get their turn.

Anyone can check the numbers: 664,579 primes (OEIS A006880), 58,980 twin-prime pairs (OEIS
A007508), and a largest gap of 154, after 4,652,353 (Wikipedia, "Prime gap").
"""

import sys
from itertools import compress
from math import isqrt

import thryve
from project_report import send_report

# The project's name, as the programs know it.
PROJECT = "primes"
# The numbers below this one are counted.
LIMIT = 10_000_000
# How many ranges the numbers are cut into, each sieved by a Sieve task of its own.
RANGES = 8
# The Sieve tasks' priority: low, so that tasks with a higher priority go first.
PRIORITY = 1
# Printed under the report's title, so that the numbers aren't taken for Thryve's own metrics.
NOTE = (
    "Note: The following numbers are calculations about the numbers below 10,000,000 "
    "and are not metrics related to Thryve itself."
)

# ============================================================================
# Finding the primes
# ============================================================================


class Found:
    """What the sieve found in a run of numbers: one range, or several in a row."""

    def __init__(self):
        self.primes = 0
        self.twin_pairs = 0
        # The largest gap between two primes in a row, and the prime it comes after; the first
        # such gap, when several are as large.
        self.largest_gap = 0
        self.gap_after = 0
        # The first and the last prime: the twin pairs and gaps across runs are found with them.
        self.first = None
        self.last = None

    def add(self, prime):
        """Counts `prime`, the next prime after the ones found so far, and the gap it ends."""
        if self.last is not None:
            self.gap(self.last, prime)
        self.primes += 1
        if self.first is None:
            self.first = prime
        self.last = prime

    def gap(self, before, prime):
        """Notes the gap between `before` and `prime`, two primes in a row: a twin pair when it's
        2, and the largest gap when no gap before it is as large."""
        gap = prime - before
        if gap == 2:
            self.twin_pairs += 1
        if gap > self.largest_gap:
            self.largest_gap = gap
            self.gap_after = before


def primes_up_to(limit):
    """The primes up to `limit`, found with a plain sieve."""
    is_prime = bytearray([0, 0]) + bytearray(b"\x01") * (limit - 1)
    for number in range(2, isqrt(limit) + 1):
        if is_prime[number]:
            multiples = range(number * number, limit + 1, number)
            is_prime[number * number :: number] = bytes(len(multiples))
    return list(compress(range(limit + 1), is_prime))


def sieve_range(start, end):
    """The primes from `start` up to `end`, found by crossing out the multiples of the primes up to
    its square root. What's left is prime."""
    is_prime = bytearray(b"\x01") * (end - start)
    for prime in primes_up_to(isqrt(end - 1)):
        # The prime's first multiple in the range, but never the prime itself.
        first = max(prime * prime, (start + prime - 1) // prime * prime)
        is_prime[first - start :: prime] = bytes(len(range(first, end, prime)))
    # 0 and 1 aren't prime.
    for number in range(start, min(2, end)):
        is_prime[number - start] = 0

    found = Found()
    for prime in compress(range(start, end), is_prime):
        found.add(prime)
    return found


def add_up(runs):
    """Adds up what runs of numbers found, in their order. A twin pair or a gap can cross from one
    run into the next: from the last prime of one to the first prime of the next."""
    total = Found()
    for run in runs:
        if total.last is not None and run.first is not None:
            total.gap(total.last, run.first)
        total.primes += run.primes
        total.twin_pairs += run.twin_pairs
        if run.largest_gap > total.largest_gap:
            total.largest_gap = run.largest_gap
            total.gap_after = run.gap_after
        if total.first is None:
            total.first = run.first
        if run.last is not None:
            total.last = run.last
    return total


# ============================================================================
# The steps, and how they're wired
# ============================================================================


def sieve(first, count):
    """Sieve: finds the primes in one task's share of the ranges, `count` ranges from `first`."""
    size = LIMIT // RANGES
    return add_up(sieve_range(n * size, (n + 1) * size) for n in range(first, first + count))


def combine(sieved):
    """Combine: adds up what the Sieve tasks found. If one failed, its error is passed on."""
    if sieved.error:
        raise sieved.error
    return add_up(sieved.values)


def write(tx, combined):
    """Write: writes the report from what Combine found, and sends it to Console. A failure is
    reported too, so that Console still gets a report from this project."""
    if combined.error:
        text = f"The primes report failed: {combined.error.message}\n"
    else:
        found = combined.value
        text = (
            f"Primes report\n{NOTE}\n"
            f"  primes: {found.primes}\n"
            f"  twin-prime pairs: {found.twin_pairs}\n"
            f"  largest gap: {found.largest_gap}, after {found.gap_after}\n"
        )
    send_report(tx, PROJECT, text)


def declare_count(tx):
    """Declares the whole count: Sieve, Combine and Write. Each step takes the handles of the steps
    before it, so the engine runs every step as soon as its input is ready. Every call uses up the
    handles it's given, even when it fails."""
    # Sieve: the numbers cut into 8 ranges, each sieved by a task of its own.
    sieved = thryve.split_into(tx, RANGES, "Sieve", RANGES, PRIORITY, sieve)

    # Combine: once all 8 are sieved, adds up what they found.
    combined = thryve.join(sieved, "Combine", combine)

    # Write: once Combine is done, sends the report to Console. Nothing follows it: its handle is
    # let go.
    thryve.free_handle(thryve.then(combined, "Write", lambda found: write(tx, found)))


class Primes:
    """A plugin that counts the primes once the engine starts."""

    def __init__(self, tx):
        self.tx = tx

    def start(self):
        """The plugin's entry point, run once, on a worker, when the engine starts."""
        try:
            declare_count(self.tx)
        except Exception as error:
            message = f"can't declare the count: {error}"
            print(message, file=sys.stderr)
            return "FinErr", message, "Error"
        return "Started", f"counting the primes below {LIMIT}", "Main"


def plugin(tx):
    """The Primes plugin."""
    return thryve.plugin("Primes", tx, Primes(tx).start)
