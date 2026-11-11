// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

//! Primes: counts the primes below 10,000,000, the twin-prime pairs among them, and the largest gap
//! between two primes in a row.
//!
//! - Primes, a plugin, cuts the numbers into 8 ranges once the engine starts. 8 Sieve tasks find
//!   the primes in one range each, at the same time.
//! - Combine waits for all 8 (a join) and adds up what they found, in the ranges' order.
//! - Write follows Combine (a `then` step): it writes the report and sends it to Console, an
//!   observer, which prints it.
//!
//! The Sieve tasks run at a low priority, 1. When other tasks wait for a worker too, those with a
//! higher priority go first; Thryve raises the priority of a task that waits, so the Sieve tasks
//! still get their turn.
//!
//! Anyone can check the numbers: 664,579 primes (OEIS A006880), 58,980 twin-prime pairs (OEIS
//! A007508), and a largest gap of 154, after 4,652,353 (Wikipedia, "Prime gap").

use std::convert::Infallible;
use std::error::Error;
use std::ops::Range;
use std::sync::Arc;

use thryve::prelude::*;
use thryve::utils::bases::plugin::Plugin;
use thryve::utils::impl_as_any;
use thryve::utils::res::TaskResult;
use thryve::utils::traits::PlgLogic;

use crate::send_report;

/// The project's name, as the programs know it.
pub const PROJECT: &str = "primes";
/// The numbers below this one are counted.
const LIMIT: u32 = 10_000_000;
/// How many ranges the numbers are cut into, each sieved by a Sieve task of its own.
const RANGES: usize = 8;
/// The Sieve tasks' priority: low, so that tasks with a higher priority go first.
const PRIORITY: u8 = 1;
/// Printed under the report's title, so that the numbers aren't taken for Thryve's own metrics.
const NOTE: &str = "Note: The following numbers are calculations about the numbers below 10,000,000 \
                    and are not metrics related to Thryve itself.";

// ============================================================================
// Finding the primes
// ============================================================================

/// What the sieve found in a run of numbers: one range, or several in a row.
#[derive(Clone, Copy, Default)]
struct Found {
    primes: u64,
    twin_pairs: u64,
    /// The largest gap between two primes in a row; the first one, when several are as large.
    largest_gap: u32,
    /// The prime the largest gap comes after.
    gap_after: u32,
    /// The first and the last prime: the twin pairs and gaps across runs are found with them.
    first: Option<u32>,
    last: Option<u32>,
}

impl Found {
    /// Counts `prime`, the next prime after the ones found so far, and the gap it ends.
    fn add(&mut self, prime: u32) {
        if let Some(before) = self.last {
            self.gap(before, prime);
        }
        self.primes += 1;
        self.first.get_or_insert(prime);
        self.last = Some(prime);
    }

    /// Notes the gap between `before` and `prime`, two primes in a row: a twin pair when it's 2,
    /// and the largest gap when no gap before it is as large.
    fn gap(&mut self, before: u32, prime: u32) {
        let gap = prime - before;
        if gap == 2 {
            self.twin_pairs += 1;
        }
        if gap > self.largest_gap {
            self.largest_gap = gap;
            self.gap_after = before;
        }
    }
}

/// The primes up to `limit`, found with a plain sieve.
fn primes_up_to(limit: u32) -> Vec<u32> {
    let mut crossed_out = vec![false; limit as usize + 1];
    let mut primes = Vec::new();
    for number in 2..=limit {
        if crossed_out[number as usize] {
            continue;
        }
        primes.push(number);
        for multiple in (number * number..=limit).step_by(number as usize) {
            crossed_out[multiple as usize] = true;
        }
    }
    primes
}

/// The primes in `range`, found by crossing out the multiples of the primes up to its square root.
/// What's left is prime.
fn sieve_range(range: Range<u32>) -> Found {
    let mut crossed_out = vec![false; range.len()];
    for prime in primes_up_to((range.end - 1).isqrt()) {
        // The prime's first multiple in the range, but never the prime itself.
        let first = (prime * prime).max(range.start.div_ceil(prime) * prime);
        for multiple in (first..range.end).step_by(prime as usize) {
            crossed_out[(multiple - range.start) as usize] = true;
        }
    }

    let mut found = Found::default();
    // 0 and 1 aren't prime.
    for number in range.start.max(2)..range.end {
        if !crossed_out[(number - range.start) as usize] {
            found.add(number);
        }
    }
    found
}

/// Adds up what runs of numbers found, in their order. A twin pair or a gap can cross from one run
/// into the next: from the last prime of one to the first prime of the next.
fn add_up(runs: Vec<Found>) -> Found {
    let mut total = Found::default();
    for run in runs {
        if let (Some(before), Some(first)) = (total.last, run.first) {
            total.gap(before, first);
        }
        total.primes += run.primes;
        total.twin_pairs += run.twin_pairs;
        if run.largest_gap > total.largest_gap {
            total.largest_gap = run.largest_gap;
            total.gap_after = run.gap_after;
        }
        total.first = total.first.or(run.first);
        total.last = run.last.or(total.last);
    }
    total
}

// ============================================================================
// The steps, and how they're wired
// ============================================================================

/// The numbers below `LIMIT`, cut into `RANGES` ranges of the same size.
fn ranges() -> Vec<Range<u32>> {
    let size = LIMIT / RANGES as u32;
    (0..RANGES as u32)
        .map(|n| n * size..(n + 1) * size)
        .collect()
}

/// Sieve: finds the primes in one task's share of the ranges. A sieve can't fail: its error type,
/// `Infallible`, has no values.
fn sieve(ranges: Vec<Range<u32>>, _first: usize) -> Result<Found, Infallible> {
    Ok(add_up(ranges.into_iter().map(sieve_range).collect()))
}

/// Combine: adds up what the Sieve tasks found. None of them can fail, so all of them are here.
fn combine(sieved: JoinHandoff<Vec<Found>, Infallible>) -> Result<Found, Infallible> {
    let JoinHandoff::Filled { values } = sieved;
    Ok(add_up(values))
}

/// Write: writes the report from what Combine found, and sends it to Console.
fn write(tx: &PacketHandle, combined: Handoff<Found, Infallible>) -> Result<(), Infallible> {
    let Handoff::Value(found) = combined;
    let text = format!(
        "Primes report\n{NOTE}\n  primes: {}\n  twin-prime pairs: {}\n  largest gap: {}, after {}\n",
        found.primes, found.twin_pairs, found.largest_gap, found.gap_after
    );
    send_report(tx, PROJECT, text);
    Ok(())
}

/// Declares the whole count: Sieve, Combine and Write. Each step takes the handles of the steps
/// before it, so the engine runs every step as soon as its input is ready.
fn declare_count(tx: &PacketHandle) -> Result<(), Box<dyn Error>> {
    // Sieve: the numbers cut into 8 ranges, each sieved by a task of its own.
    let sieved = tx.split_into(ranges(), "Sieve", RANGES, PRIORITY, sieve)?;

    // Combine: once all 8 are sieved, adds up what they found.
    let combined = sieved.join("Combine", combine)?;

    // Write: once Combine is done, sends the report to Console.
    let tx = tx.clone();
    combined.then("Write", move |found| write(&tx, found))?;
    Ok(())
}

/// Primes: a plugin that counts the primes once the engine starts.
pub struct Primes;

impl Primes {
    /// The Primes plugin.
    pub fn plugin(tx: PacketHandle) -> Plugin {
        let sender = tx.clone();
        Plugin::new("Primes", Arc::new(Self), tx, move || Self::start(&sender))
            .expect("the plugin's type is valid")
    }

    /// The plugin's entry point, run once, on a worker, when the engine starts.
    fn start(tx: &PacketHandle) -> TaskResult {
        match declare_count(tx) {
            Ok(()) => TaskResult::new(
                "Started",
                format!("counting the primes below {LIMIT}"),
                "Main",
            ),
            Err(error) => {
                let message = format!("can't declare the count: {error}");
                eprintln!("{message}");
                TaskResult::new("FinErr", message, "Error")
            }
        }
    }
}

impl PlgLogic for Primes {
    fn clean_up(&self) -> Result<(), ThryveError> {
        Ok(())
    }

    impl_as_any!(Primes);
}
