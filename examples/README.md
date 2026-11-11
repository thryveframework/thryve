# Examples

Small programs that show how Thryve is used. Thryve works from any language that can call a C
library; these examples come in the four its documentation covers: Rust, C, C++ and Python. Every
version of an example prints the same output from the same input.

## The access-log report

A web server writes a line to its access log for every request it answers: who asked, when,
which page, how the request ended (its status), how big the response was in bytes, and how long
it took in milliseconds.

```text
203.0.113.244 - - [09/Nov/2026:12:00:00 +0000] "GET / HTTP/1.1" 200 10096 11
```

The example reads [`data/access.log`](data/access.log), 10,000 requests to a made-up website, and
reports on them. Its numbers are worked out from the log's lines: they describe that website's
traffic, not Thryve's own performance.

```text
Access log report
Note: The following numbers are calculations from the log file and are not metrics related to Thryve itself.
  requests: 10000
  data sent in responses: 131.9 MB
  lines skipped (not requests): 3

Status codes
  200    9195   92.0%
  301      50    0.5%
  304     384    3.8%
  404     295    3.0%
  500      76    0.8%

Busiest pages
  /                       1784 requests     23 ms on average
  /api/tasks              1192 requests    195 ms on average
  /docs                    978 requests     29 ms on average
  /docs/getting-started    809 requests     34 ms on average
  /api/search              768 requests    740 ms on average

Alerts
  /api/search           slow: 740 ms on average
  /api/tasks            server errors: 6.4% of its requests
```

How Thryve runs it:

- **LogReader**, a plugin, reads the log once the engine starts, and cuts its lines into 8
  pieces. 8 **Parse** tasks count one piece each, at the same time.
- **Merge** waits for all 8 (a join) and adds their counts together.
- **Totals**, **StatusCodes**, **TopPages** and **Alerts** each write one section of the report
  from Merge's counts, which they share rather than copy (a fan-out).
- **Report** waits for the four sections (another join) and sends the text to **Console**, an
  observer, which prints it and ends the run.

LogReader and Console never name each other: the report reaches Console as a notification.

Every number is worked out with whole numbers, rounded half up, and every list is sorted the same
way, so all four languages print the same report from the same log.

## Two projects in one engine

One engine can run several projects at the same time. The `projects` program runs the access-log
report next to a second, unrelated project, Primes, which counts the primes below 10,000,000. It
prints the access-log report, then:

```text
Primes report
Note: The following numbers are calculations about the numbers below 10,000,000 and are not metrics related to Thryve itself.
  primes: 664579
  twin-prime pairs: 58980
  largest gap: 154, after 4652353
```

Anyone can check these: OEIS's [A006880](https://oeis.org/A006880) and
[A007508](https://oeis.org/A007508), and the table of record gaps on Wikipedia's
[prime gap](https://en.wikipedia.org/wiki/Prime_gap) page.

How Thryve runs Primes:

- **Primes**, a plugin, cuts the numbers into 8 ranges once the engine starts. 8 **Sieve** tasks
  find the primes in one range each, at the same time.
- **Combine** waits for all 8 (a join) and adds up what they found.
- **Write** follows Combine (a `then` step) and sends the report to **Console**.

The two projects share the engine's workers. The Sieve tasks run at a lower priority than the
report's tasks, so when tasks of both wait for a worker, the report's go first. Thryve raises the
priority of a task that waits, so the Sieve tasks still get their turn.

Neither project names the other. Console waits for a report from each, prints them in the order
the projects were named, and ends the run, so the output is the same on every run.

To switch projects, name the ones to run after the program: `primes` runs only Primes,
`access-log` only the report, and `primes access-log` both, with Primes' report first. A project
that can't start, like the report without its log, ends the run for both: its `FinErr` stops the
engine, not just its own project.

## Running them

The programs find the sample log and Thryve's configuration, [`data/thryve.toml`](data/thryve.toml),
on their own, so they run from any folder. `access_log` takes another log as its argument; without
one, it reads `data/access.log`.

Every language splits the code the same way: the report (`log_report`), Primes (`primes`),
Console (`console`), the notification that carries a project's report to Console, which the
projects and Console share, and the two programs (`access_log` and `projects`).

### Rust

From anywhere in the repository:

```text
cargo run -p thryve_examples --bin access_log
cargo run -p thryve_examples --bin projects
```

Name projects after `--`: `cargo run -p thryve_examples --bin projects -- primes`. The code is in
[`rust/`](rust/); the shared notification is in [`rust/lib.rs`](rust/lib.rs).

### C and C++

They need Thryve's C library: build it from this repository with `cargo build -p thryve_ffi`, or
download `thryve_ffi-<version>-<platform>` from the
[latest release](https://github.com/thryveframework/thryve/releases/latest) and unzip it.

With CMake, from the repository's root:

```text
cmake -S examples/c_cpp -B examples/c_cpp/build
cmake --build examples/c_cpp/build
```

For a release download, add `-DTHRYVE_FFI_DIR=<the unzipped folder>` to the first command. The
programs are `access_log_c`, `projects_c`, `access_log_cpp` and `projects_cpp`, in
`examples/c_cpp/build/` (in `examples/c_cpp/build/Debug/` with Visual Studio), with a copy of the
DLL next to them.

**Without the DLL.** With MSVC, CMake can build Thryve into each program instead. This needs this
repository's build of the library: a release download holds only the DLL.

```text
cmake -S examples/c_cpp -B examples/c_cpp/build-static -DTHRYVE_FFI_STATIC=ON
cmake --build examples/c_cpp/build-static
```

With GNU Make and gcc (MSYS2's, on Windows), from the repository's root: `make -C examples/c_cpp`,
or `make -C examples/c_cpp THRYVE_FFI_DIR=<the unzipped folder>` for a release download. The
programs are in `examples/c_cpp/build/`.

The code is in [`c_cpp/c/`](c_cpp/c/) and [`c_cpp/cpp/`](c_cpp/cpp/); the shared notification is
in `common`.

### Python

Python 3.8 or later, with nothing to install: [`python/thryve.py`](python/thryve.py) is Thryve's
C API for Python, through `ctypes`. It finds the C library in this repository's build, or where
the `THRYVE_FFI` environment variable points: the unzipped release download, or the library itself.
It always uses the shared library (the DLL on Windows): `ctypes` can't load a static one.

```text
cargo build -p thryve_ffi
python examples/python/access_log.py
python examples/python/projects.py
```

The code is in [`python/`](python/); the shared notification is in
[`python/project_report.py`](python/project_report.py). The
[Python guide](../crates/thryve_ffi/docs/python-dev-guide.md) shows how to write programs like
these, with `thryve.py`.

## What Thryve writes

Each run writes its logs (`Main.log`, `Error.log` and `Debug.log`) in a `log/` folder next to the
program: in `target/debug/` for Rust, in the build folder for C and C++. Python's go in
`python/out/`, with the copy of the configuration it runs with, since the folder next to its
program is Python's own.

Thryve prints lines of its own around the reports: `loaded plugin: <its type>` as it loads each
plugin, and one at the end about the run's task graph. Graph mode is on in `data/thryve.toml`, so
Thryve writes the graph to `topology/topology_export.ron`, next to the logs, and opens it in
[Topology](https://github.com/thryveframework/thryve-topology) if `Topology.exe` is next to the
program. When it isn't, the last line says where Thryve looked.

## The sample log

[`data/make_access_log.py`](data/make_access_log.py) writes `data/access.log`, and writes the
same file every time. Among the 10,003 lines are 3 that aren't requests, which the report counts
as skipped.
