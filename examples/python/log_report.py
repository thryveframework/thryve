# Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
#
# This file is part of Thryve, dual-licensed under the GNU Affero General
# Public License v3.0 (or later) and a separate Commercial License.
# See LICENSE and COMMERCIAL.md in the repository root, or visit
# https://thryve-framework.com for licensing options.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The access-log report: Thryve reads a web server's access log and reports on it.

- LogReader, a plugin, reads the log once the engine starts, and cuts its lines into 8 pieces.
  8 Parse tasks count one piece each, at the same time.
- Merge waits for all 8 (a join) and adds their counts together.
- Totals, StatusCodes, TopPages and Alerts each write one section of the report from Merge's
  counts, which they share rather than copy (a fan-out).
- Report waits for the four sections (another join) and sends the text to Console, an observer,
  which prints it.

LogReader and Console never name each other: the report reaches Console as a notification.
"""

import sys
from collections import Counter, defaultdict

import thryve
from project_report import send_report

# The project's name, as the programs know it.
PROJECT = "access-log"
# How many pieces the log is cut into, each counted by a Parse task of its own.
PIECES = 8
# The Parse tasks' priority. When tasks wait for a worker, the higher priority goes first.
PRIORITY = 2
# How many of the busiest pages the report lists.
TOP_PAGES = 5
# A page is slow when its requests take this long on average, in milliseconds.
SLOW_MS = 500
# A page fails too often when this share of its requests end in a server error, in tenths of a
# percent.
FAILING_TENTHS = 50
# Printed under the report's title, so that the log's numbers aren't taken for Thryve's own metrics.
NOTE = (
    "Note: The following numbers are calculations from the log file and are not metrics "
    "related to Thryve itself."
)

# ============================================================================
# Counting the log
# ============================================================================


class Page:
    """One page's requests."""

    def __init__(self):
        self.requests = 0
        self.total_ms = 0
        self.server_errors = 0


class Counts:
    """What a piece of the log holds, counted."""

    def __init__(self):
        self.requests = 0
        self.skipped = 0  # lines that aren't requests
        self.bytes = 0
        self.statuses = Counter()  # requests by status
        self.pages = defaultdict(Page)  # requests by page


def number(word):
    """A whole number: one or more ASCII digits, which fit in 64 bits. None for anything else."""
    if not (word.isascii() and word.isdigit()):
        return None
    value = int(word)
    return value if value < 2**64 else None


def read_request(line):
    """Reads one line of the log: the page asked for, the status, the size in bytes and the time
    in milliseconds. None for a line that isn't a request."""
    # The request is between the first two quotes: the method, the page and the protocol.
    _, quote, rest = line.partition('"')
    request, closing, rest = rest.partition('"')
    asked = request.split()
    if not quote or not closing or len(asked) < 3:
        return None

    # After it come the status, the size and the time.
    fields = [number(field) for field in rest.split()[:3]]
    if len(fields) < 3 or None in fields:
        return None
    return (asked[1], *fields)


def parse(lines, first, count):
    """Parse: counts the requests in one piece of the log, `count` lines from `first`."""
    counts = Counts()
    for line in lines[first : first + count]:
        request = read_request(line)
        if request is None:
            counts.skipped += 1
            continue
        path, status, size, ms = request
        counts.requests += 1
        counts.bytes += size
        counts.statuses[status] += 1

        page = counts.pages[path]
        page.requests += 1
        page.total_ms += ms
        if 500 <= status < 600:
            page.server_errors += 1
    return counts


def merge(counted):
    """Merge: adds the pieces' counts together. If a piece failed, its error is passed on."""
    if counted.error:
        raise counted.error
    total = Counts()
    for piece in counted.values:
        total.requests += piece.requests
        total.skipped += piece.skipped
        total.bytes += piece.bytes
        total.statuses.update(piece.statuses)
        for path, page in piece.pages.items():
            summed = total.pages[path]
            summed.requests += page.requests
            summed.total_ms += page.total_ms
            summed.server_errors += page.server_errors
    return total


def read_lines(path):
    """Reads the log's lines the way Rust's `str::lines` does: each ends at a '\\n', without the
    '\\r' before it, and a last line needs no '\\n'."""
    with open(path, encoding="utf-8", newline="") as file:
        pieces = file.read().split("\n")
    last = pieces.pop()  # what follows the last '\n': a line only if there's something there
    lines = [line[:-1] if line.endswith("\r") else line for line in pieces]
    if last:
        lines.append(last)
    return lines


# ============================================================================
# Writing the report. Every number is worked out with whole numbers, rounded half up, so every
# language prints the same report.
# ============================================================================


def tenths(part, whole):
    """`part` out of `whole`, in tenths of a percent; 0 when `whole` is."""
    return (part * 1000 + whole // 2) // whole if whole else 0


def percent(share):
    """Tenths of a percent, written as a percent with one decimal."""
    return f"{share // 10}.{share % 10}%"


def average(total, count):
    """The average of `total` over `count`; 0 when `count` is."""
    return (total + count // 2) // count if count else 0


def totals(counts):
    """Totals: the report's title and note, then how many requests there were, how much data the
    server sent back, and how many lines weren't requests."""
    # Each request's line holds the size of its response, so their sum is what the server sent to
    # its visitors, not the size of the log.
    megabyte_tenths = (counts.bytes * 10 + 524288) // 1048576
    return (
        f"Access log report\n{NOTE}\n"
        f"  requests: {counts.requests}\n"
        f"  data sent in responses: {megabyte_tenths // 10}.{megabyte_tenths % 10} MB\n"
        f"  lines skipped (not requests): {counts.skipped}\n"
    )


def status_codes(counts):
    """StatusCodes: how many requests ended with each status."""
    text = "Status codes\n"
    for status in sorted(counts.statuses):
        requests = counts.statuses[status]
        share = percent(tenths(requests, counts.requests))
        text += f"  {status}  {requests:>6}  {share:>6}\n"
    return text


def top_pages(counts):
    """TopPages: the busiest pages, and how long their requests take on average."""
    # The most requests first; pages with as many, in order of their path.
    pages = sorted(counts.pages.items(), key=lambda item: (-item[1].requests, item[0]))
    text = "Busiest pages\n"
    for path, page in pages[:TOP_PAGES]:
        ms = average(page.total_ms, page.requests)
        text += f"  {path:<22}{page.requests:>6} requests  {ms:>5} ms on average\n"
    return text


def alerts(counts):
    """Alerts: the pages that are slow, or that fail too often."""
    lines = ""
    for path in sorted(counts.pages):
        page = counts.pages[path]
        ms = average(page.total_ms, page.requests)
        if ms >= SLOW_MS:
            lines += f"  {path:<22}slow: {ms} ms on average\n"
        failing = tenths(page.server_errors, page.requests)
        if failing >= FAILING_TENTHS:
            lines += f"  {path:<22}server errors: {percent(failing)} of its requests\n"
    return "Alerts\n" + (lines or "  none\n")


def section(write):
    """A section's job: writes the section with `write`, from Merge's counts. Every section reads
    the same counts, so none may change them. If Merge failed, its error is passed on."""

    def job(counts):
        if counts.error:
            raise counts.error
        return write(counts.value)

    return job


# ============================================================================
# The tasks, and how they're wired
# ============================================================================


def report(tx, written):
    """Report: joins the four sections, in this order, and sends them to Console. A failure is
    reported too, so that Console still gets a report from this project."""
    if written.error:
        text = f"The report failed: {written.error.message}\n"
    else:
        text = "\n".join(written.values)
    send_report(tx, PROJECT, text)


def declare_report(tx, lines):
    """Declares the whole report: Parse, Merge, the four sections and Report. Each step takes the
    handles of the steps before it, so the engine runs every step as soon as its input is ready.
    Every call uses up the handles it's given, even when it fails."""
    # Parse: the log cut into 8 pieces, each counted by a task of its own.
    pieces = thryve.split_into(
        tx, len(lines), "Parse", PIECES, PRIORITY, lambda first, count: parse(lines, first, count)
    )

    # Merge: once all 8 are counted, adds their counts together.
    merged = thryve.join(pieces, "Merge", merge)

    # The four sections, each written from Merge's counts, which they share.
    group = thryve.fan_out(merged)
    sections = [
        thryve.fanout_then(group, "Totals", section(totals)),
        thryve.fanout_then(group, "StatusCodes", section(status_codes)),
        thryve.fanout_then(group, "TopPages", section(top_pages)),
        thryve.fanout_then(group, "Alerts", section(alerts)),
    ]
    try:
        thryve.fanout_send(group)
    except thryve.Error:
        for handle in sections:
            thryve.free_handle(handle)
        raise

    # Report: once the four are written, sends them to Console. Nothing follows it: its handle is
    # let go.
    thryve.free_handle(thryve.join(sections, "Report", lambda written: report(tx, written)))


class LogReader:
    """A plugin that reads the log once the engine starts, and declares the report."""

    def __init__(self, log, tx):
        self.log = log
        self.tx = tx

    def start(self):
        """The plugin's entry point, run once, on a worker, when the engine starts."""
        # Without a log there's nothing to report on: a FinErr result ends the run.
        try:
            lines = read_lines(self.log)
        except (OSError, ValueError) as error:
            message = f"can't read {self.log}: {getattr(error, 'strerror', None) or error}"
            print(message, file=sys.stderr)
            return "FinErr", message, "Error"

        try:
            declare_report(self.tx, lines)
        except Exception as error:
            message = f"can't declare the report: {error}"
            print(message, file=sys.stderr)
            return "FinErr", message, "Error"
        return "Started", f"reading {self.log}", "Main"


def plugin(log, tx):
    """The LogReader plugin, which will read `log`."""
    return thryve.plugin("LogReader", tx, LogReader(log, tx).start)
