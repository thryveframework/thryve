// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

//! The access-log report: Thryve reads a web server's access log and reports on it.
//!
//! - LogReader, a plugin, reads the log once the engine starts, and cuts its lines into 8 pieces.
//!   8 Parse tasks count one piece each, at the same time.
//! - Merge waits for all 8 (a join) and adds their counts together.
//! - Totals, StatusCodes, TopPages and Alerts each write one section of the report from Merge's
//!   counts, which they share rather than copy (a fan-out).
//! - Report waits for the four sections (another join) and sends the text to Console, an observer,
//!   which prints it.
//!
//! LogReader and Console never name each other: the report reaches Console as a notification.

use std::collections::BTreeMap;
use std::error::Error;
use std::io;
use std::path::PathBuf;
use std::sync::Arc;

use thryve::prelude::*;
use thryve::utils::bases::plugin::Plugin;
use thryve::utils::impl_as_any;
use thryve::utils::res::TaskResult;
use thryve::utils::traits::PlgLogic;

use crate::send_report;

/// The project's name, as the programs know it.
pub const PROJECT: &str = "access-log";
/// How many pieces the log is cut into, each counted by a Parse task of its own.
const PIECES: usize = 8;
/// The Parse tasks' priority. When tasks wait for a worker, the higher priority goes first.
const PRIORITY: u8 = 2;
/// How many of the busiest pages the report lists.
const TOP_PAGES: usize = 5;
/// A page is slow when its requests take this long on average, in milliseconds.
const SLOW_MS: u64 = 500;
/// A page fails too often when this share of its requests end in a server error, in tenths of a
/// percent.
const FAILING_TENTHS: u64 = 50;
/// Printed under the report's title, so that the log's numbers aren't taken for Thryve's own metrics.
const NOTE: &str = "Note: The following numbers are calculations from the log file and are not metrics \
                    related to Thryve itself.";

// ============================================================================
// Counting the log
// ============================================================================

/// One page's requests.
#[derive(Default)]
struct Page {
    requests: u64,
    total_ms: u64,
    server_errors: u64,
}

/// What a piece of the log holds, counted.
#[derive(Default)]
struct Counts {
    requests: u64,
    /// Lines that aren't requests.
    skipped: u64,
    bytes: u64,
    /// Requests by status, in the order of the status.
    statuses: BTreeMap<u64, u64>,
    /// Requests by page, in byte order of the page's path.
    pages: BTreeMap<String, Page>,
}

/// A whole number: one or more ASCII digits.
fn number(word: &str) -> Option<u64> {
    if word.is_empty() || !word.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    word.parse().ok()
}

/// Reads one line of the log: the page asked for, the status, the size in bytes and the time in
/// milliseconds. `None` for a line that isn't a request.
fn read_request(line: &str) -> Option<(&str, u64, u64, u64)> {
    // The request is between the first two quotes: the method, the page and the protocol.
    let (_, rest) = line.split_once('"')?;
    let (request, rest) = rest.split_once('"')?;
    let mut request = request.split_whitespace();
    let (_method, page, _protocol) = (request.next()?, request.next()?, request.next()?);

    // After it come the status, the size and the time.
    let mut fields = rest.split_whitespace();
    let status = number(fields.next()?)?;
    let bytes = number(fields.next()?)?;
    let ms = number(fields.next()?)?;
    Some((page, status, bytes, ms))
}

/// Parse: counts the requests in one piece of the log. Counting can't fail; `io::Error` is the error
/// type the report's steps would pass on to Report.
fn parse(lines: Vec<String>, _first: usize) -> Result<Counts, io::Error> {
    let mut counts = Counts::default();
    for line in &lines {
        let Some((path, status, bytes, ms)) = read_request(line) else {
            counts.skipped += 1;
            continue;
        };
        counts.requests += 1;
        counts.bytes += bytes;
        *counts.statuses.entry(status).or_default() += 1;

        let page = counts.pages.entry(path.to_owned()).or_default();
        page.requests += 1;
        page.total_ms += ms;
        if (500..600).contains(&status) {
            page.server_errors += 1;
        }
    }
    Ok(counts)
}

/// Merge: adds the pieces' counts together. If a piece failed, its error is passed on.
fn merge(counted: JoinHandoff<Vec<Counts>, io::Error>) -> Result<Counts, io::Error> {
    let pieces = match counted {
        JoinHandoff::Filled { values } => values,
        JoinHandoff::Error { error, .. } => return Err(error),
    };
    let mut total = Counts::default();
    for piece in pieces {
        total.requests += piece.requests;
        total.skipped += piece.skipped;
        total.bytes += piece.bytes;
        for (status, requests) in piece.statuses {
            *total.statuses.entry(status).or_default() += requests;
        }
        for (path, page) in piece.pages {
            let sum = total.pages.entry(path).or_default();
            sum.requests += page.requests;
            sum.total_ms += page.total_ms;
            sum.server_errors += page.server_errors;
        }
    }
    Ok(total)
}

// ============================================================================
// Writing the report. Every number is worked out with whole numbers, rounded half up, so every
// language prints the same report.
// ============================================================================

/// `part` out of `whole`, in tenths of a percent; 0 when `whole` is.
fn tenths(part: u64, whole: u64) -> u64 {
    (part * 1000 + whole / 2).checked_div(whole).unwrap_or(0)
}

/// Tenths of a percent, written as a percent with one decimal.
fn percent(tenths: u64) -> String {
    format!("{}.{}%", tenths / 10, tenths % 10)
}

/// The average of `total` over `count`; 0 when `count` is.
fn average(total: u64, count: u64) -> u64 {
    (total + count / 2).checked_div(count).unwrap_or(0)
}

/// Totals: the report's title and note, then how many requests there were, how much data the server
/// sent back, and how many lines weren't requests.
fn totals(counts: &Counts) -> String {
    // Each request's line holds the size of its response, so their sum is what the server sent to
    // its visitors, not the size of the log.
    let megabyte_tenths = (counts.bytes * 10 + 524_288) / 1_048_576;
    format!(
        "Access log report\n{NOTE}\n  requests: {}\n  data sent in responses: {}.{} MB\n  \
         lines skipped (not requests): {}\n",
        counts.requests,
        megabyte_tenths / 10,
        megabyte_tenths % 10,
        counts.skipped
    )
}

/// StatusCodes: how many requests ended with each status.
fn status_codes(counts: &Counts) -> String {
    let mut text = String::from("Status codes\n");
    for (status, requests) in &counts.statuses {
        let share = percent(tenths(*requests, counts.requests));
        text += &format!("  {status}  {requests:>6}  {share:>6}\n");
    }
    text
}

/// TopPages: the busiest pages, and how long their requests take on average.
fn top_pages(counts: &Counts) -> String {
    // The most requests first; pages with as many, in byte order of their path.
    let mut pages: Vec<(&String, &Page)> = counts.pages.iter().collect();
    pages.sort_by(|a, b| b.1.requests.cmp(&a.1.requests).then(a.0.cmp(b.0)));

    let mut text = String::from("Busiest pages\n");
    for (path, page) in pages.into_iter().take(TOP_PAGES) {
        let ms = average(page.total_ms, page.requests);
        text += &format!(
            "  {path:<22}{:>6} requests  {ms:>5} ms on average\n",
            page.requests
        );
    }
    text
}

/// Alerts: the pages that are slow, or that fail too often.
fn alerts(counts: &Counts) -> String {
    let mut lines = String::new();
    for (path, page) in &counts.pages {
        let ms = average(page.total_ms, page.requests);
        if ms >= SLOW_MS {
            lines += &format!("  {path:<22}slow: {ms} ms on average\n");
        }
        let failing = tenths(page.server_errors, page.requests);
        if failing >= FAILING_TENTHS {
            lines += &format!(
                "  {path:<22}server errors: {} of its requests\n",
                percent(failing)
            );
        }
    }
    if lines.is_empty() {
        lines = String::from("  none\n");
    }
    format!("Alerts\n{lines}")
}

/// Writes one section with `write`, from Merge's counts. If Merge failed, its error is passed on.
fn section(
    counts: &Handoff<Counts, Arc<io::Error>>,
    write: fn(&Counts) -> String,
) -> Result<String, Arc<io::Error>> {
    match counts {
        Handoff::Value(counts) => Ok(write(counts)),
        Handoff::Error(error) => Err(Arc::clone(error)),
    }
}

// ============================================================================
// The tasks, and how they're wired
// ============================================================================

/// Report: joins the four sections, in this order, and sends them to Console. A failure is reported
/// too, so that Console still gets a report from this project.
fn report(
    tx: &PacketHandle,
    written: JoinHandoff<Vec<String>, Arc<io::Error>>,
) -> Result<(), Arc<io::Error>> {
    let text = match written {
        JoinHandoff::Filled { values } => values.join("\n"),
        JoinHandoff::Error { error, .. } => format!("The report failed: {error}\n"),
    };
    send_report(tx, PROJECT, text);
    Ok(())
}

/// Declares the whole report: Parse, Merge, the four sections and Report. Each step takes the
/// handles of the steps before it, so the engine runs every step as soon as its input is ready.
fn declare_report(tx: &PacketHandle, lines: Vec<String>) -> Result<(), Box<dyn Error>> {
    // Parse: the log cut into 8 pieces, each counted by a task of its own.
    let pieces = tx.split_into(lines, "Parse", PIECES, PRIORITY, parse)?;

    // Merge: once all 8 are counted, adds their counts together.
    let merged = pieces.join("Merge", merge)?;

    // The four sections, each written from Merge's counts, which they share.
    let mut group = merged.fan_out();
    let sections = vec![
        group.then("Totals", |counts| section(&counts, totals))?,
        group.then("StatusCodes", |counts| section(&counts, status_codes))?,
        group.then("TopPages", |counts| section(&counts, top_pages))?,
        group.then("Alerts", |counts| section(&counts, alerts))?,
    ];
    group.send_fanout()?;

    // Report: once the four are written, sends them to Console.
    let tx = tx.clone();
    sections.join("Report", move |written| report(&tx, written))?;
    Ok(())
}

/// LogReader: a plugin that reads the log once the engine starts, and declares the report.
pub struct LogReader {
    log: PathBuf,
}

impl LogReader {
    /// The LogReader plugin, which will read `log`.
    pub fn plugin(log: PathBuf, tx: PacketHandle) -> Plugin {
        let core = Arc::new(Self { log });
        let reader = Arc::clone(&core);
        let sender = tx.clone();
        Plugin::new("LogReader", core, tx, move || reader.start(&sender))
            .expect("the plugin's type is valid")
    }

    /// The plugin's entry point, run once, on a worker, when the engine starts.
    fn start(&self, tx: &PacketHandle) -> TaskResult {
        // Without a log there's nothing to report on: a `FinErr` result ends the run.
        let text = match std::fs::read_to_string(&self.log) {
            Ok(text) => text,
            Err(error) => {
                let message = format!("can't read {}: {error}", self.log.display());
                eprintln!("{message}");
                return TaskResult::new("FinErr", message, "Error");
            }
        };

        let lines = text.lines().map(str::to_owned).collect();
        match declare_report(tx, lines) {
            Ok(()) => TaskResult::new("Started", format!("reading {}", self.log.display()), "Main"),
            Err(error) => {
                let message = format!("can't declare the report: {error}");
                eprintln!("{message}");
                TaskResult::new("FinErr", message, "Error")
            }
        }
    }
}

impl PlgLogic for LogReader {
    fn clean_up(&self) -> Result<(), ThryveError> {
        Ok(())
    }

    impl_as_any!(LogReader);
}
