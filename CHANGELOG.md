# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Examples, in `examples/`: one program, a report on a web server's access log, written in
  Rust, C, C++ and Python. Each prints the same report from the same sample log. They use a
  plugin, an observer, a split, two joins and a fan-out.
- `examples/python/thryve.py`: the C API for Python, through `ctypes`. It runs on the C
  library from the 0.1.0 release.
- A guide for Python developers, `crates/thryve_ffi/docs/python-dev-guide.md`: plugins,
  observers and typed tasks in Python, through `thryve.py`.
- A second example, `projects`, in the same four languages: two unrelated projects in one
  engine at the same time, the access-log report and a count of the primes below 10,000,000.
- The C and C++ examples can have Thryve built into them, without the DLL:
  `-DTHRYVE_FFI_STATIC=ON`, with MSVC and this repository's build of the C library.

### Changed

- The C and C++ examples, with their CMake and Make files, moved from `crates/thryve_ffi` to
  `examples/c_cpp`. They build against this repository's build of the C library, or against a
  release download.

## [0.1.0] - 2026-11-09

The first release of Thryve, a task engine for Rust and C: plugins send tasks, observers
receive notifications, and the engine runs the tasks on a pool of workers.

### Added

- Tasks with priorities, and aging, so that a low-priority task is never starved.
- Plugins and observers, which work together without knowing each other.
- Typed tasks (`AdvTask`): a task's value, or its own error, goes to the follow-up declared
  with `then`. A task can also hand its outcome on to another task (forwarding).
- Joins (`join`): several tasks share one follow-up. Splits (`split_into`, `send_split_tasks`):
  one job is cut over several tasks. Fan-out (`fan_out`): several follow-ups share one result.
- Cancelling (`cancel`): a task that hasn't started never runs, and neither does the work
  that only it needed.
- Two ways to run: interactively, or as a Windows service with pause, resume and stop.
- Logs in a folder per run (`Main.log`, `Error.log`, `Debug.log`, and your own files), next
  to the program or in a root folder you choose (`root_dir`).
- A graceful shutdown, with a grace period for running tasks and one line that counts any
  lost work.
- A debug mode, with metrics and graph mode: the topology handler records which task sent
  which, and Topology shows the graph at shutdown.
- Configuration in a TOML file, with defaults; bad values are refused together when it loads.
- The C API (`thryve_ffi`), with C and C++ headers: plugins, observers, plain and typed
  tasks, follow-ups, joins, splits, fan-out, forwarding and cancelling.
- Downloads on the release page for Windows (x86_64): the C library, Topology, or both, each
  as .zip and .tar.gz.
- Rust 1.98 or later.

### Known limitations

- A panic in any task ends the whole process. Panic isolation is planned.
- Windows only, for now.
- Called from Python, Thryve looks for `Topology.exe` next to `python.exe`.

[0.1.0]: https://github.com/thryveframework/thryve/releases/tag/v0.1.0
