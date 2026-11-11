// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// The access-log report: Thryve reads a web server's access log and reports on it. See
// log_report.hpp.

#include "log_report.hpp"

#include "common.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// How many pieces the log is cut into, each counted by a Parse task of its own.
constexpr std::size_t PIECES = 8;
// The Parse tasks' priority. When tasks wait for a worker, the higher priority goes first.
constexpr std::uint8_t PRIORITY = 2;
// How many of the busiest pages the report lists.
constexpr std::size_t TOP_PAGES = 5;
// A page is slow when its requests take this long on average, in milliseconds.
constexpr std::uint64_t SLOW_MS = 500;
// A page fails too often when this share of its requests end in a server error, in tenths of a
// percent.
constexpr std::uint64_t FAILING_TENTHS = 50;
// Printed under the report's title, so that the log's numbers aren't taken for Thryve's own metrics.
constexpr const char* NOTE =
    "Note: The following numbers are calculations from the log file and are not metrics "
    "related to Thryve itself.";

// ============================================================================
// Counting the log
// ============================================================================

// The log's lines.
using Lines = std::vector<std::string>;

// One page's requests.
struct Page {
    std::uint64_t requests = 0;
    std::uint64_t total_ms = 0;
    std::uint64_t server_errors = 0;
};

// What a piece of the log holds, counted.
struct Counts {
    std::uint64_t requests = 0;
    std::uint64_t skipped = 0;  // lines that aren't requests
    std::uint64_t bytes = 0;
    std::map<std::uint64_t, std::uint64_t> statuses;  // requests by status, in the status's order
    std::map<std::string, Page> pages;                 // requests by page, in byte order of the path
};

// A space, a tab, or one of the other ASCII whitespace characters.
bool is_space(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

// Reads a text's words one at a time.
class Words {
public:
    explicit Words(std::string_view text) : text_(text) {}

    // The next word; empty once there's none left.
    std::string_view next() {
        std::size_t start = 0;
        while (start < text_.size() && is_space(text_[start])) {
            ++start;
        }
        std::size_t stop = start;
        while (stop < text_.size() && !is_space(text_[stop])) {
            ++stop;
        }
        std::string_view word = text_.substr(start, stop - start);
        text_.remove_prefix(stop);
        return word;
    }

private:
    std::string_view text_;
};

// A whole number: one or more ASCII digits, which fit in 64 bits.
bool number(std::string_view word, std::uint64_t& value) {
    if (word.empty()) {
        return false;
    }

    std::uint64_t result = 0;
    for (char c : word) {
        if (c < '0' || c > '9') {
            return false;
        }
        auto digit = static_cast<std::uint64_t>(c - '0');
        if (result > (UINT64_MAX - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    value = result;
    return true;
}

// One request from the log: the page asked for, the status, the size in bytes and the time in
// milliseconds.
struct Request {
    std::string_view page;
    std::uint64_t status = 0;
    std::uint64_t bytes = 0;
    std::uint64_t ms = 0;
};

// Reads one line of the log. False for a line that isn't a request.
bool read_request(std::string_view line, Request& request) {
    // The request is between the first two quotes: the method, the page and the protocol.
    std::size_t open = line.find('"');
    if (open == std::string_view::npos) {
        return false;
    }
    std::size_t close = line.find('"', open + 1);
    if (close == std::string_view::npos) {
        return false;
    }
    Words asked(line.substr(open + 1, close - open - 1));
    std::string_view method = asked.next();
    request.page = asked.next();
    std::string_view protocol = asked.next();
    if (method.empty() || request.page.empty() || protocol.empty()) {
        return false;
    }

    // After it come the status, the size and the time.
    Words fields(line.substr(close + 1));
    return number(fields.next(), request.status) && number(fields.next(), request.bytes) &&
           number(fields.next(), request.ms);
}

// Parse: counts the requests in `count` lines of the log, from `first`.
Counts count_lines(const Lines& lines, std::size_t first, std::size_t count) {
    Counts counts;
    for (std::size_t i = first; i < first + count; ++i) {
        Request request;
        if (!read_request(lines[i], request)) {
            ++counts.skipped;
            continue;
        }
        ++counts.requests;
        counts.bytes += request.bytes;
        ++counts.statuses[request.status];

        Page& page = counts.pages[std::string(request.page)];
        ++page.requests;
        page.total_ms += request.ms;
        if (request.status >= 500 && request.status < 600) {
            ++page.server_errors;
        }
    }
    return counts;
}

// Merge: adds a piece's counts to `total`.
void add(Counts& total, const Counts& piece) {
    total.requests += piece.requests;
    total.skipped += piece.skipped;
    total.bytes += piece.bytes;
    for (const auto& [status, requests] : piece.statuses) {
        total.statuses[status] += requests;
    }
    for (const auto& [path, page] : piece.pages) {
        Page& sum = total.pages[path];
        sum.requests += page.requests;
        sum.total_ms += page.total_ms;
        sum.server_errors += page.server_errors;
    }
}

// Reads the log's lines the way Rust's `str::lines` does: each ends at a '\n', without the '\r'
// before it, and a last line needs no '\n'.
Lines read_lines(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("can't read " + path + ": " + std::strerror(errno));
    }

    Lines lines;
    std::string line;
    while (std::getline(file, line)) {
        // Before the end of the file, the line had its '\n'.
        if (!file.eof() && !line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    if (file.bad()) {
        throw std::runtime_error("can't read " + path);
    }
    return lines;
}

// ============================================================================
// Writing the report. Every number is worked out with whole numbers, rounded half up, so every
// language prints the same report.
// ============================================================================

// `part` out of `whole`, in tenths of a percent; 0 when `whole` is.
std::uint64_t tenths(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0 : (part * 1000 + whole / 2) / whole;
}

// Tenths of a percent, written as a percent with one decimal.
std::string percent(std::uint64_t share) {
    return std::to_string(share / 10) + "." + std::to_string(share % 10) + "%";
}

// The average of `total` over `count`; 0 when `count` is.
std::uint64_t average(std::uint64_t total, std::uint64_t count) {
    return count == 0 ? 0 : (total + count / 2) / count;
}

// Totals: the report's title and note, then how many requests there were, how much data the server
// sent back, and how many lines weren't requests.
std::string totals(const Counts& counts) {
    // Each request's line holds the size of its response, so their sum is what the server sent to
    // its visitors, not the size of the log.
    std::uint64_t megabyte_tenths = (counts.bytes * 10 + 524288) / 1048576;

    std::ostringstream text;
    text << "Access log report\n"
         << NOTE << "\n"
         << "  requests: " << counts.requests << "\n"
         << "  data sent in responses: " << megabyte_tenths / 10 << "." << megabyte_tenths % 10
         << " MB\n"
         << "  lines skipped (not requests): " << counts.skipped << "\n";
    return text.str();
}

// StatusCodes: how many requests ended with each status.
std::string status_codes(const Counts& counts) {
    std::ostringstream text;
    text << "Status codes\n";
    for (const auto& [status, requests] : counts.statuses) {
        text << "  " << status << "  " << std::setw(6) << requests << "  " << std::setw(6)
             << percent(tenths(requests, counts.requests)) << "\n";
    }
    return text.str();
}

// TopPages: the busiest pages, and how long their requests take on average.
std::string top_pages(const Counts& counts) {
    // The most requests first; pages with as many, in byte order of their path.
    std::vector<const std::pair<const std::string, Page>*> pages;
    for (const auto& entry : counts.pages) {
        pages.push_back(&entry);
    }
    std::sort(pages.begin(), pages.end(), [](const auto* a, const auto* b) {
        if (a->second.requests != b->second.requests) {
            return a->second.requests > b->second.requests;
        }
        return a->first < b->first;
    });

    std::ostringstream text;
    text << "Busiest pages\n";
    for (std::size_t i = 0; i < pages.size() && i < TOP_PAGES; ++i) {
        const auto& [path, page] = *pages[i];
        text << "  " << std::left << std::setw(22) << path << std::right << std::setw(6)
             << page.requests << " requests  " << std::setw(5)
             << average(page.total_ms, page.requests) << " ms on average\n";
    }
    return text.str();
}

// Alerts: the pages that are slow, or that fail too often.
std::string alerts(const Counts& counts) {
    std::ostringstream lines;
    lines << std::left;
    for (const auto& [path, page] : counts.pages) {
        std::uint64_t ms = average(page.total_ms, page.requests);
        if (ms >= SLOW_MS) {
            lines << "  " << std::setw(22) << path << "slow: " << ms << " ms on average\n";
        }
        std::uint64_t failing = tenths(page.server_errors, page.requests);
        if (failing >= FAILING_TENTHS) {
            lines << "  " << std::setw(22) << path << "server errors: " << percent(failing)
                  << " of its requests\n";
        }
    }
    std::string text = lines.str();
    return "Alerts\n" + (text.empty() ? std::string("  none\n") : text);
}

// ============================================================================
// The tasks, and how they're wired
// ============================================================================

// Parse: counts the requests in one piece of the log, `count` lines from `first`.
ThryveOutcome parse(std::size_t first, std::size_t count, void* lines) noexcept {
    return guarded(
        [&] { return value(count_lines(*static_cast<const Lines*>(lines), first, count)); });
}

// Lets the log's lines go, once the last Parse task is done with them.
void delete_lines(void* lines) {
    delete static_cast<Lines*>(lines);
}

// Merge: adds the pieces' counts together. If a piece failed, its error is passed on.
ThryveOutcome merge(ThryveHandoff counted, void*) noexcept {
    return guarded([&] {
        if (thryve_handoff_is_error(counted)) {
            return pass_error_on(counted);
        }
        Counts total;
        for (std::size_t i = 0; i < thryve_handoff_value_count(counted); ++i) {
            add(total, read_value<Counts>(counted, i));
        }
        return value(std::move(total));
    });
}

// A section's job: writes the section with `Write`, from Merge's counts. If Merge failed, its
// error is passed on.
template <std::string (*Write)(const Counts&)>
ThryveOutcome section(ThryveHandoff counts, void*) noexcept {
    return guarded([&] {
        if (thryve_handoff_is_error(counts)) {
            return pass_error_on(counts);
        }
        return value(Write(read_value<Counts>(counts)));
    });
}

// Report: joins the four sections, in this order, and sends them to Console. A failure is reported
// too, so that Console still gets a report from this project. Its user data is the engine's packet
// handle.
ThryveOutcome report(ThryveHandoff written, void* tx) noexcept {
    return guarded([&] {
        std::string text;
        if (thryve_handoff_is_error(written)) {
            text = "The report failed: " + handoff_error(written) + "\n";
        } else {
            for (std::size_t i = 0; i < thryve_handoff_value_count(written); ++i) {
                text += (i == 0 ? "" : "\n") + read_value<std::string>(written, i);
            }
        }
        send_report(static_cast<ThryvePacketHandle>(tx), LOG_REPORT_PROJECT, std::move(text));
        return thryve_outcome_value(nullptr, nullptr);
    });
}

// Declares the whole report: Parse, Merge, the four sections and Report. Each step takes the
// handles of the steps before it, so the engine runs every step as soon as its input is ready.
// Every call uses up the handles it's given, even when it fails. Returns which step couldn't be
// declared, and why; empty once all of them are.
std::string declare_report(ThryvePacketHandle tx, Lines lines) {
    // Parse: the log cut into 8 pieces, each counted by a task of its own. They share the lines,
    // which Thryve deletes once the last one is done, or right away if this fails.
    auto* shared = new Lines(std::move(lines));
    ThryveAdvHandle pieces[PIECES];
    std::size_t piece_count = 0;
    if (thryve_packet_handle_split_into(tx, shared->size(), "Parse", PIECES, PRIORITY, parse,
                                        shared, delete_lines, pieces,
                                        &piece_count) != ThryveStatus_Ok) {
        return "Parse: " + last_error();
    }

    // Merge: once all 8 are counted, adds their counts together.
    ThryveAdvHandle merged =
        thryve_adv_handles_join(pieces, piece_count, "Merge", merge, nullptr, nullptr);
    if (merged == nullptr) {
        return "Merge: " + last_error();
    }

    // The four sections, each written from Merge's counts, which they share.
    ThryveFanoutGroup group = thryve_adv_handle_fan_out(merged);
    if (group == nullptr) {
        return "the sections: " + last_error();
    }
    ThryveAdvHandle sections[] = {
        thryve_fanout_then(group, "Totals", section<totals>, nullptr, nullptr),
        thryve_fanout_then(group, "StatusCodes", section<status_codes>, nullptr, nullptr),
        thryve_fanout_then(group, "TopPages", section<top_pages>, nullptr, nullptr),
        thryve_fanout_then(group, "Alerts", section<alerts>, nullptr, nullptr),
    };
    if (thryve_fanout_send(group) != ThryveStatus_Ok) {
        std::string why = "the sections: " + last_error();
        for (ThryveAdvHandle handle : sections) {
            if (handle != nullptr) {
                thryve_adv_handle_free(handle);
            }
        }
        return why;
    }

    // Report: once the four are written, sends them to Console.
    ThryveAdvHandle reported =
        thryve_adv_handles_join(sections, 4, "Report", report, tx, nullptr);
    if (reported == nullptr) {
        return "Report: " + last_error();
    }

    // Nothing follows Report: let its handle go.
    thryve_adv_handle_free(reported);
    return {};
}

// The plugin's entry point, run once, on a worker, when the engine starts.
void start(ThryveTaskResult* result, void* core) noexcept {
    auto& reader = *static_cast<LogReader*>(core);
    try {
        // Without a log there's nothing to report on: a FinErr result ends the run.
        std::string failed = declare_report(reader.tx, read_lines(reader.log));
        if (!failed.empty()) {
            throw std::runtime_error("can't declare " + failed);
        }
        reader.message = "reading " + reader.log;
        *result = thryve_task_result_new("Started", reader.message.c_str(), "Main");
    } catch (const std::exception& error) {
        reader.message = error.what();
        std::cerr << reader.message << "\n";
        *result = thryve_task_result_new("FinErr", reader.message.c_str(), "Error");
    }
}

// LogReader has nothing to clean up when the engine stops.
int clean_up_log_reader(const void*) noexcept {
    return ThryveStatus_Ok;
}

}  // namespace

ThryvePlugin log_reader_plugin(LogReader& reader) {
    return thryve_plugin_create("LogReader", &reader, reader.tx, clean_up_log_reader, start);
}
