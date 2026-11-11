// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// The access-log report: Thryve reads a web server's access log and reports on it. See
// log_report.h.

#include "log_report.h"

#include "common.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// How many pieces the log is cut into, each counted by a Parse task of its own.
#define PIECES 8
// The Parse tasks' priority. When tasks wait for a worker, the higher priority goes first.
#define PRIORITY 2
// How many of the busiest pages the report lists.
#define TOP_PAGES 5
// A page is slow when its requests take this long on average, in milliseconds.
#define SLOW_MS 500
// A page fails too often when this share of its requests end in a server error, in tenths of a
// percent.
#define FAILING_TENTHS 50
// Printed under the report's title, so that the log's numbers aren't taken for Thryve's own metrics.
#define NOTE                                                                              \
    "Note: The following numbers are calculations from the log file and are not metrics " \
    "related to Thryve itself."

// ============================================================================
// Counting the log
// ============================================================================

// One page's requests.
typedef struct {
    char* path;
    size_t path_length;
    uint64_t requests;
    uint64_t total_ms;
    uint64_t server_errors;
} Page;

// How many requests ended with one status.
typedef struct {
    uint64_t status;
    uint64_t requests;
} Status;

// What a piece of the log holds, counted. The statuses are kept in their order, and the pages in
// byte order of their path.
typedef struct {
    uint64_t requests;
    uint64_t skipped;  // lines that aren't requests
    uint64_t bytes;
    Status* statuses;
    size_t status_count;
    size_t status_room;
    Page* pages;
    size_t page_count;
    size_t page_room;
} Counts;

// Frees a Counts; Thryve calls it for the values Parse and Merge return.
static void free_counts(void* counts) {
    Counts* freed = counts;
    for (size_t i = 0; i < freed->page_count; i++) {
        free(freed->pages[i].path);
    }
    free(freed->pages);
    free(freed->statuses);
    free(freed);
}

// Makes room for one more item in an array of `size`-byte items. False when out of memory.
static bool make_room(void** items, size_t count, size_t* room, size_t size) {
    if (count < *room) {
        return true;
    }
    size_t new_room = *room == 0 ? 16 : *room * 2;
    void* grown = realloc(*items, new_room * size);
    if (grown == NULL) {
        return false;
    }
    *items = grown;
    *room = new_room;
    return true;
}

// Finds a status's count, adding it if it's new. NULL when out of memory.
static Status* find_status(Counts* counts, uint64_t status) {
    // The statuses are in order: a binary search finds it, or where it goes.
    size_t low = 0;
    size_t high = counts->status_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (counts->statuses[middle].status == status) {
            return &counts->statuses[middle];
        }
        if (counts->statuses[middle].status < status) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (!make_room((void**)&counts->statuses, counts->status_count, &counts->status_room,
                   sizeof *counts->statuses)) {
        return NULL;
    }
    memmove(&counts->statuses[low + 1], &counts->statuses[low],
            (counts->status_count - low) * sizeof *counts->statuses);
    counts->statuses[low] = (Status){status, 0};
    counts->status_count++;
    return &counts->statuses[low];
}

// Compares a page's path with `length` bytes at `path`, in byte order.
static int compare_path(const Page* page, const char* path, size_t length) {
    size_t shorter = page->path_length < length ? page->path_length : length;
    int order = memcmp(page->path, path, shorter);
    if (order != 0) {
        return order;
    }
    return (page->path_length > length) - (page->path_length < length);
}

// Finds the page whose path is `length` bytes at `path`, adding it if it's new. NULL when out of
// memory.
static Page* find_page(Counts* counts, const char* path, size_t length) {
    // The pages are in byte order of their path: a binary search finds it, or where it goes.
    size_t low = 0;
    size_t high = counts->page_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = compare_path(&counts->pages[middle], path, length);
        if (order == 0) {
            return &counts->pages[middle];
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    // The page keeps a copy of its path: the log it came from is freed after Parse.
    char* copy = malloc(length + 1);
    if (copy == NULL ||
        !make_room((void**)&counts->pages, counts->page_count, &counts->page_room,
                   sizeof *counts->pages)) {
        free(copy);
        return NULL;
    }
    memcpy(copy, path, length);
    copy[length] = '\0';

    memmove(&counts->pages[low + 1], &counts->pages[low],
            (counts->page_count - low) * sizeof *counts->pages);
    counts->pages[low] = (Page){copy, length, 0, 0, 0};
    counts->page_count++;
    return &counts->pages[low];
}

// A space, a tab, or one of the other ASCII whitespace characters.
static bool is_space(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

// Finds the next word between `*at` and `end`, and moves `*at` past it. False if there's none.
static bool next_word(const char** at, const char* end, const char** word, size_t* length) {
    const char* start = *at;
    while (start < end && is_space(*start)) {
        start++;
    }
    if (start == end) {
        return false;
    }

    const char* stop = start;
    while (stop < end && !is_space(*stop)) {
        stop++;
    }
    *word = start;
    *length = (size_t)(stop - start);
    *at = stop;
    return true;
}

// A whole number: one or more ASCII digits, which fit in 64 bits.
static bool number(const char* word, size_t length, uint64_t* value) {
    if (length == 0) {
        return false;
    }

    uint64_t result = 0;
    for (size_t i = 0; i < length; i++) {
        if (word[i] < '0' || word[i] > '9') {
            return false;
        }
        unsigned digit = (unsigned)(word[i] - '0');
        if (result > (UINT64_MAX - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

// One request from the log: the page asked for, the status, the size in bytes and the time in
// milliseconds.
typedef struct {
    const char* page;
    size_t page_length;
    uint64_t status;
    uint64_t bytes;
    uint64_t ms;
} Request;

// Reads one line of the log, `length` bytes at `line`. False for a line that isn't a request.
static bool read_request(const char* line, size_t length, Request* request) {
    const char* end = line + length;
    const char* word;
    size_t word_length;

    // The request is between the first two quotes: the method, the page and the protocol.
    const char* open = memchr(line, '"', length);
    if (open == NULL) {
        return false;
    }
    const char* close = memchr(open + 1, '"', (size_t)(end - open - 1));
    if (close == NULL) {
        return false;
    }
    const char* at = open + 1;
    if (!next_word(&at, close, &word, &word_length) ||
        !next_word(&at, close, &request->page, &request->page_length) ||
        !next_word(&at, close, &word, &word_length)) {
        return false;
    }

    // After it come the status, the size and the time.
    at = close + 1;
    return next_word(&at, end, &word, &word_length) &&
           number(word, word_length, &request->status) &&
           next_word(&at, end, &word, &word_length) &&
           number(word, word_length, &request->bytes) &&
           next_word(&at, end, &word, &word_length) && number(word, word_length, &request->ms);
}

// Counts one line of the log. False when out of memory.
static bool count_line(Counts* counts, const char* line, size_t length) {
    Request request;
    if (!read_request(line, length, &request)) {
        counts->skipped++;
        return true;
    }
    counts->requests++;
    counts->bytes += request.bytes;

    Status* status = find_status(counts, request.status);
    Page* page = status != NULL ? find_page(counts, request.page, request.page_length) : NULL;
    if (page == NULL) {
        return false;
    }
    status->requests++;
    page->requests++;
    page->total_ms += request.ms;
    if (request.status >= 500 && request.status < 600) {
        page->server_errors++;
    }
    return true;
}

// Adds a piece's counts to `total`. False when out of memory.
static bool add_counts(Counts* total, const Counts* piece) {
    total->requests += piece->requests;
    total->skipped += piece->skipped;
    total->bytes += piece->bytes;

    for (size_t i = 0; i < piece->status_count; i++) {
        Status* status = find_status(total, piece->statuses[i].status);
        if (status == NULL) {
            return false;
        }
        status->requests += piece->statuses[i].requests;
    }

    for (size_t i = 0; i < piece->page_count; i++) {
        const Page* page = &piece->pages[i];
        Page* sum = find_page(total, page->path, page->path_length);
        if (sum == NULL) {
            return false;
        }
        sum->requests += page->requests;
        sum->total_ms += page->total_ms;
        sum->server_errors += page->server_errors;
    }
    return true;
}

// ============================================================================
// Reading the log
// ============================================================================

// One line: where it starts in the log's text, and how many bytes it has.
typedef struct {
    size_t start;
    size_t length;
} Line;

// The log, read whole, and its lines. Every Parse task reads it; Thryve frees it once the last
// one is done.
typedef struct {
    char* text;
    Line* lines;
    size_t line_count;
} Log;

static void free_log(void* log) {
    Log* freed = log;
    free(freed->text);
    free(freed->lines);
    free(freed);
}

// Reads the whole file at `path`. NULL if it can't, with errno saying why.
static char* read_file(const char* path, size_t* length) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    char* text = NULL;
    size_t room = 0;
    *length = 0;
    for (;;) {
        if (room - *length < 65536) {
            room = room == 0 ? 65536 : room * 2;
            char* grown = realloc(text, room);
            if (grown == NULL) {
                free(text);
                fclose(file);
                errno = ENOMEM;
                return NULL;
            }
            text = grown;
        }

        size_t got = fread(text + *length, 1, room - *length, file);
        *length += got;
        if (got == 0) {
            break;
        }
    }

    int error = ferror(file) ? errno : 0;
    fclose(file);
    if (error != 0) {
        free(text);
        errno = error;
        return NULL;
    }
    return text;
}

// Reads the log at `path`, and finds its lines the way Rust's `str::lines` does: each ends at a
// '\n', without the '\r' before it, and a last line needs no '\n'. NULL if it can't be read, with
// errno saying why.
static Log* read_log(const char* path) {
    size_t length;
    char* text = read_file(path, &length);
    if (text == NULL) {
        return NULL;
    }

    size_t line_count = 0;
    for (size_t i = 0; i < length; i++) {
        line_count += text[i] == '\n';
    }
    if (length > 0 && text[length - 1] != '\n') {
        line_count++;
    }

    Log* log = malloc(sizeof *log);
    Line* lines = malloc((line_count > 0 ? line_count : 1) * sizeof *lines);
    if (log == NULL || lines == NULL) {
        free(text);
        free(log);
        free(lines);
        errno = ENOMEM;
        return NULL;
    }

    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\n') {
            size_t stop = i > start && text[i - 1] == '\r' ? i - 1 : i;
            lines[count++] = (Line){start, stop - start};
            start = i + 1;
        }
    }
    if (start < length) {
        lines[count++] = (Line){start, length - start};
    }

    *log = (Log){text, lines, line_count};
    return log;
}

// ============================================================================
// Writing the report. Every number is worked out with whole numbers, rounded half up, so every
// language prints the same report.
// ============================================================================

// `part` out of `whole`, in tenths of a percent; 0 when `whole` is.
static uint64_t tenths(uint64_t part, uint64_t whole) {
    return whole == 0 ? 0 : (part * 1000 + whole / 2) / whole;
}

// Tenths of a percent, written into `out` as a percent with one decimal.
static void percent(uint64_t share, char* out, size_t size) {
    snprintf(out, size, "%llu.%llu%%", (unsigned long long)(share / 10),
             (unsigned long long)(share % 10));
}

// The average of `total` over `count`; 0 when `count` is.
static uint64_t average(uint64_t total, uint64_t count) {
    return count == 0 ? 0 : (total + count / 2) / count;
}

// Totals: the report's title and note, then how many requests there were, how much data the server
// sent back, and how many lines weren't requests.
static char* totals(const Counts* counts) {
    // Each request's line holds the size of its response, so their sum is what the server sent to
    // its visitors, not the size of the log.
    unsigned long long megabyte_tenths = (counts->bytes * 10 + 524288) / 1048576;

    Text text = {0};
    text_add(&text, "Access log report\n" NOTE "\n");
    text_add(&text, "  requests: %llu\n", (unsigned long long)counts->requests);
    text_add(&text, "  data sent in responses: %llu.%llu MB\n", megabyte_tenths / 10,
             megabyte_tenths % 10);
    text_add(&text, "  lines skipped (not requests): %llu\n", (unsigned long long)counts->skipped);
    return text_finish(&text);
}

// StatusCodes: how many requests ended with each status.
static char* status_codes(const Counts* counts) {
    Text text = {0};
    text_add(&text, "Status codes\n");
    for (size_t i = 0; i < counts->status_count; i++) {
        const Status* status = &counts->statuses[i];
        char share[32];
        percent(tenths(status->requests, counts->requests), share, sizeof share);
        text_add(&text, "  %llu  %6llu  %6s\n", (unsigned long long)status->status,
                 (unsigned long long)status->requests, share);
    }
    return text_finish(&text);
}

// The most requests first; pages with as many, in byte order of their path.
static int busiest_first(const void* a, const void* b) {
    const Page* first = *(const Page* const*)a;
    const Page* second = *(const Page* const*)b;
    if (first->requests != second->requests) {
        return first->requests > second->requests ? -1 : 1;
    }
    return compare_path(first, second->path, second->path_length);
}

// TopPages: the busiest pages, and how long their requests take on average.
static char* top_pages(const Counts* counts) {
    const Page** pages = malloc((counts->page_count > 0 ? counts->page_count : 1) * sizeof *pages);
    if (pages == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < counts->page_count; i++) {
        pages[i] = &counts->pages[i];
    }
    qsort((void*)pages, counts->page_count, sizeof *pages, busiest_first);

    Text text = {0};
    text_add(&text, "Busiest pages\n");
    for (size_t i = 0; i < counts->page_count && i < TOP_PAGES; i++) {
        unsigned long long ms = average(pages[i]->total_ms, pages[i]->requests);
        text_add(&text, "  %-22s%6llu requests  %5llu ms on average\n", pages[i]->path,
                 (unsigned long long)pages[i]->requests, ms);
    }
    free((void*)pages);
    return text_finish(&text);
}

// Alerts: the pages that are slow, or that fail too often.
static char* alerts(const Counts* counts) {
    Text text = {0};
    text_add(&text, "Alerts\n");
    bool any = false;
    for (size_t i = 0; i < counts->page_count; i++) {
        const Page* page = &counts->pages[i];
        unsigned long long ms = average(page->total_ms, page->requests);
        if (ms >= SLOW_MS) {
            text_add(&text, "  %-22sslow: %llu ms on average\n", page->path, ms);
            any = true;
        }
        uint64_t failing = tenths(page->server_errors, page->requests);
        if (failing >= FAILING_TENTHS) {
            char share[32];
            percent(failing, share, sizeof share);
            text_add(&text, "  %-22sserver errors: %s of its requests\n", page->path, share);
            any = true;
        }
    }
    if (!any) {
        text_add(&text, "  none\n");
    }
    return text_finish(&text);
}

// ============================================================================
// The tasks, and how they're wired
// ============================================================================

// Parse: counts the requests in one piece of the log, `count` lines from `first`.
static ThryveOutcome parse(size_t first, size_t count, void* log) {
    const Log* lines = log;
    Counts* counts = calloc(1, sizeof *counts);
    if (counts == NULL) {
        return out_of_memory();
    }
    for (size_t i = first; i < first + count; i++) {
        const Line* line = &lines->lines[i];
        if (!count_line(counts, lines->text + line->start, line->length)) {
            free_counts(counts);
            return out_of_memory();
        }
    }
    return thryve_outcome_value(counts, free_counts);
}

// Merge: adds the pieces' counts together. If a piece failed, its error is passed on.
static ThryveOutcome merge(ThryveHandoff counted, void* user_data) {
    (void)user_data;
    if (thryve_handoff_is_error(counted)) {
        return pass_error_on(counted);
    }

    Counts* total = calloc(1, sizeof *total);
    if (total == NULL) {
        return out_of_memory();
    }
    size_t pieces = thryve_handoff_value_count(counted);
    for (size_t i = 0; i < pieces; i++) {
        if (!add_counts(total, thryve_handoff_value(counted, i))) {
            free_counts(total);
            return out_of_memory();
        }
    }
    return thryve_outcome_value(total, free_counts);
}

// Writes one section with `write`, from Merge's counts. Every section reads the same counts in
// place, so none may change them. If Merge failed, its error is passed on.
static ThryveOutcome section(ThryveHandoff counts, char* (*write)(const Counts*)) {
    if (thryve_handoff_is_error(counts)) {
        return pass_error_on(counts);
    }
    char* text = write(thryve_handoff_value(counts, 0));
    return text != NULL ? thryve_outcome_value(text, free) : out_of_memory();
}

// The four sections' jobs.
static ThryveOutcome totals_job(ThryveHandoff counts, void* user_data) {
    (void)user_data;
    return section(counts, totals);
}

static ThryveOutcome status_codes_job(ThryveHandoff counts, void* user_data) {
    (void)user_data;
    return section(counts, status_codes);
}

static ThryveOutcome top_pages_job(ThryveHandoff counts, void* user_data) {
    (void)user_data;
    return section(counts, top_pages);
}

static ThryveOutcome alerts_job(ThryveHandoff counts, void* user_data) {
    (void)user_data;
    return section(counts, alerts);
}

// Report: joins the four sections, in this order, and sends them to Console. A failure is reported
// too, so that Console still gets a report from this project. Its user data is the engine's packet
// handle.
static ThryveOutcome report(ThryveHandoff written, void* tx) {
    Text text = {0};
    if (thryve_handoff_is_error(written)) {
        text_add(&text, "The report failed: %s\n", handoff_error(written));
    } else {
        size_t count = thryve_handoff_value_count(written);
        for (size_t i = 0; i < count; i++) {
            const char* part = thryve_handoff_value(written, i);
            text_add(&text, "%s%s", i == 0 ? "" : "\n", part);
        }
    }
    char* report_text = text_finish(&text);
    if (report_text == NULL) {
        return out_of_memory();
    }
    send_report(tx, LOG_REPORT_PROJECT, report_text);
    return thryve_outcome_value(NULL, NULL);
}

// Notes why a step couldn't be declared, before another call to Thryve replaces the error.
static bool declare_failed(LogReader* reader, const char* step) {
    snprintf(reader->message, sizeof reader->message, "can't declare %s: %s", step, last_error());
    return false;
}

// Declares the whole report: Parse, Merge, the four sections and Report. Each step takes the
// handles of the steps before it, so the engine runs every step as soon as its input is ready.
// Every call uses up the handles it's given, even when it fails.
static bool declare_report(LogReader* reader, Log* log) {
    // Parse: the log cut into 8 pieces, each counted by a task of its own. They share the log,
    // which Thryve frees once the last one is done, or right away if this fails.
    ThryveAdvHandle pieces[PIECES];
    size_t piece_count = 0;
    if (thryve_packet_handle_split_into(reader->tx, log->line_count, "Parse", PIECES, PRIORITY,
                                        parse, log, free_log, pieces,
                                        &piece_count) != ThryveStatus_Ok) {
        return declare_failed(reader, "Parse");
    }

    // Merge: once all 8 are counted, adds their counts together.
    ThryveAdvHandle merged =
        thryve_adv_handles_join(pieces, piece_count, "Merge", merge, NULL, NULL);
    if (merged == NULL) {
        return declare_failed(reader, "Merge");
    }

    // The four sections, each written from Merge's counts, which they share.
    ThryveFanoutGroup group = thryve_adv_handle_fan_out(merged);
    if (group == NULL) {
        return declare_failed(reader, "the sections");
    }
    ThryveAdvHandle sections[4];
    sections[0] = thryve_fanout_then(group, "Totals", totals_job, NULL, NULL);
    sections[1] = thryve_fanout_then(group, "StatusCodes", status_codes_job, NULL, NULL);
    sections[2] = thryve_fanout_then(group, "TopPages", top_pages_job, NULL, NULL);
    sections[3] = thryve_fanout_then(group, "Alerts", alerts_job, NULL, NULL);
    if (thryve_fanout_send(group) != ThryveStatus_Ok) {
        declare_failed(reader, "the sections");
        for (size_t i = 0; i < 4; i++) {
            if (sections[i] != NULL) {
                thryve_adv_handle_free(sections[i]);
            }
        }
        return false;
    }

    // Report: once the four are written, sends them to Console.
    ThryveAdvHandle reported =
        thryve_adv_handles_join(sections, 4, "Report", report, reader->tx, NULL);
    if (reported == NULL) {
        return declare_failed(reader, "Report");
    }

    // Nothing follows Report: let its handle go.
    thryve_adv_handle_free(reported);
    return true;
}

// The plugin's entry point, run once, on a worker, when the engine starts.
static void start(ThryveTaskResult* result, void* core) {
    LogReader* reader = core;

    // Without a log there's nothing to report on: a FinErr result ends the run.
    Log* log = read_log(reader->log);
    if (log == NULL) {
        snprintf(reader->message, sizeof reader->message, "can't read %s: %s", reader->log,
                 strerror(errno));
        fprintf(stderr, "%s\n", reader->message);
        *result = thryve_task_result_new("FinErr", reader->message, "Error");
        return;
    }

    if (!declare_report(reader, log)) {
        fprintf(stderr, "%s\n", reader->message);
        *result = thryve_task_result_new("FinErr", reader->message, "Error");
        return;
    }
    snprintf(reader->message, sizeof reader->message, "reading %s", reader->log);
    *result = thryve_task_result_new("Started", reader->message, "Main");
}

// LogReader has nothing to clean up when the engine stops.
static int clean_up_log_reader(const void* core) {
    (void)core;
    return ThryveStatus_Ok;
}

ThryvePlugin log_reader_plugin(LogReader* reader) {
    return thryve_plugin_create("LogReader", reader, reader->tx, clean_up_log_reader, start);
}
