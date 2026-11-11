# Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
#
# This file is part of Thryve, dual-licensed under the GNU Affero General
# Public License v3.0 (or later) and a separate Commercial License.
# See LICENSE and COMMERCIAL.md in the repository root, or visit
# https://thryve-framework.com for licensing options.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Writes access.log, the sample web-server log the examples read.

Every run writes the same file: the random numbers come from a fixed seed. The addresses are from the
ranges set aside for documentation (RFC 5737), so none of them is real.

Each line is in the Common Log Format, with the time the request took, in milliseconds, at the end:
    203.0.113.7 - - [09/Nov/2026:12:00:00 +0000] "GET /docs HTTP/1.1" 200 5123 42
"""

import random
from pathlib import Path

REQUESTS = 10_000
rng = random.Random(2026)

# Each page: how often it's asked for, its sizes in bytes, and its times in milliseconds.
PAGES = [
    ("/", 18, (8_000, 12_000), (5, 40)),
    ("/docs", 10, (15_000, 25_000), (8, 50)),
    ("/docs/getting-started", 9, (20_000, 35_000), (8, 60)),
    ("/docs/configuration", 5, (18_000, 30_000), (8, 60)),
    ("/blog", 6, (6_000, 10_000), (5, 30)),
    ("/blog/thryve-0-1-0", 7, (12_000, 20_000), (6, 45)),
    ("/download", 6, (4_000, 6_000), (5, 30)),
    ("/api/tasks", 12, (200, 4_000), (20, 120)),
    ("/api/search", 8, (500, 6_000), (300, 1_200)),
    ("/api/stats", 4, (300, 2_000), (15, 90)),
    ("/static/app.js", 7, (60_000, 90_000), (1, 10)),
    ("/static/style.css", 6, (20_000, 30_000), (1, 8)),
    ("/favicon.ico", 2, (1_000, 1_500), (1, 4)),
    ("/wp-login.php", 3, (150, 200), (1, 5)),
]
WEIGHTS = [page[1] for page in PAGES]


def request_line(index):
    """The line of request `index`: who asked, when, for what, and how it went."""
    path, _, sizes, times = rng.choices(PAGES, WEIGHTS)[0]
    address = f"{rng.choice(['192.0.2', '198.51.100', '203.0.113'])}.{rng.randint(1, 254)}"
    seconds = index * 3 // 10
    when = f"09/Nov/2026:{12 + seconds // 3600:02}:{seconds // 60 % 60:02}:{seconds % 60:02} +0000"
    method = "POST" if path == "/api/tasks" and rng.random() < 0.4 else "GET"
    size, took, status = rng.randint(*sizes), rng.randint(*times), 200

    if path == "/wp-login.php":
        status = 404  # Bots looking for a WordPress login.
    elif path == "/api/tasks" and rng.random() < 0.06:
        status, size, took = 500, 0, rng.randint(1_000, 3_000)  # A failing backend: errors after a wait.
    elif path.startswith("/static/") and rng.random() < 0.3:
        status, size = 304, 0  # The browser's cached copy is still good.
    elif path == "/docs" and rng.random() < 0.05:
        status, size = 301, 0  # An old link, moved for good.

    return f'{address} - - [{when}] "{method} {path} HTTP/1.1" {status} {size} {took}'


lines = [request_line(index) for index in range(REQUESTS)]

# Three lines a parser can't read, as real logs have: no quotes, a status that isn't a number, a cut line.
lines.insert(2_500, "203.0.113.9 - - [09/Nov/2026:12:12:30 +0000] GET / HTTP/1.1 200 9000 12")
lines.insert(5_000, '198.51.100.4 - - [09/Nov/2026:12:25:00 +0000] "GET /docs HTTP/1.1" OK 9000 12')
lines.insert(7_500, '192.0.2.77 - - [09/Nov/2026:12:37:30 +0000] "GET /blog HTTP/1.1"')

output = Path(__file__).with_name("access.log")
output.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
print(f"Wrote {len(lines)} lines to {output}")
