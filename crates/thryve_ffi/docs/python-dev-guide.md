# Thryve FFI — Python Developer Guide

A guide for Python developers who want to use the Thryve framework through its C library.

Python reaches the C library through [`thryve.py`](../../../examples/python/thryve.py), one file that uses `ctypes`, from Python's standard library. You write plugins, observers and jobs as plain Python, and `thryve.py` carries your Python objects through Thryve for you.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [Project Setup](#project-setup)
- [Loading the Library](#loading-the-library)
- [Configuration and the Root Folder](#configuration-and-the-root-folder)
- [Writing Your First Plugin](#writing-your-first-plugin)
- [Writing Your First Observer](#writing-your-first-observer)
- [Stopping the Engine](#stopping-the-engine)
- [Writing Typed Tasks](#writing-typed-tasks)
- [Common Patterns](#common-patterns)
- [Threads and the GIL](#threads-and-the-gil)
- [What thryve.py Holds for You](#what-thryvepy-holds-for-you)
- [Beyond thryve.py](#beyond-thryvepy)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites

- **Windows (x86_64)**: Thryve builds on Windows only, for now
- **64-bit Python 3.8 or later**: the C library is 64-bit, so a 32-bit Python can't load it
- **Thryve's C library**: from the release page, or built from source with the Rust toolchain

There's nothing to install with pip: `thryve.py` needs only Python's standard library.

---

## Installation

### The C library

**Option 1: a release.** Download it from Thryve's [release page](https://github.com/thryveframework/thryve/releases/latest), as .zip or .tar.gz:

- `thryve_ffi-<version>-windows-x86_64` has the C library. Python needs only `bin/thryve_ffi.dll`; the headers and the import library are for C and C++.
- `thryve-<version>-windows-x86_64` also has `Topology.exe`, which shows the task graph in graph mode.

**Option 2: building from source.**

```bash
git clone https://github.com/thryveframework/thryve.git
cd thryve
cargo build --package thryve_ffi            # target/debug/thryve_ffi.dll
cargo build --package thryve_ffi --release  # target/release/thryve_ffi.dll
```

### thryve.py

Copy [`examples/python/thryve.py`](../../../examples/python/thryve.py) into your project.

---

## Project Setup

```
my_thryve_app/
├── thryve.py      # Copied from examples/python/
├── thryve.toml    # Thryve's configuration
├── main.py        # Your program
└── out/           # Thryve's root folder: each run's logs go here
```

Run it as any Python program:

```bash
python main.py
```

---

## Loading the Library

```python
import thryve

thryve.load()
```

`thryve.load()` opens the C library, once, and returns `thryve.lib`. Call it before anything else in `thryve`. It looks for the library:

1. Where you say: `thryve.load(r"C:\libs\thryve")`. That's the library's own path, or a folder: the library's folder, or a release unzipped there (it looks in its `bin\`).
2. Otherwise, where the `THRYVE_FFI` environment variable says, read the same way.
3. Otherwise, in the repository's build, `target\debug` and then `target\release`. That works only while `thryve.py` is still in the repository's `examples\python\`.

If none has it, `load()` raises `thryve.Error` with the places it looked.

---

## Configuration and the Root Folder

Thryve keeps each run's logs, and in graph mode the task graph, in its root folder. Without one, that's the program's own folder; for Python, the program is `python.exe`, so the logs would go into Python's installation. Give Thryve a root folder of its own.

The C library has no call that sets the root folder, so `thryve.config_from_file` sets it through the file:

```python
from pathlib import Path

HERE = Path(__file__).resolve().parent

config = thryve.config_from_file(HERE / "thryve.toml", root_dir=HERE / "out")
```

With `root_dir`, it:

- creates the folder if it isn't there;
- writes a copy of the file into it, with `root_dir` set;
- loads that copy.

Without `root_dir`, it loads the file as it is. Use a full path, like `HERE / "out"` above. A relative one is read from the working folder, so it changes with where the program is run from.

The file must set every key. Start from the [default configuration](../../../README.md#default-configuration) in Thryve's README, which also says what each key does. Keep its `root_dir` line: `config_from_file` fills it in, and raises `thryve.Error` for a file without one. `thryve.lib.thryve_config_default()` gives the defaults without a file, but with no root folder.

Each run writes its logs into `<root folder>\log\<year>_<month>_<day>_<hour>_<minute>\`:

- `Main.log`;
- `Error.log`;
- `Debug.log`, in debug mode;
- any log files of your own (see [Log files of your own](#log-files-of-your-own)).

---

## Writing Your First Plugin

A plugin starts work once the engine starts. Here is one, and the lines that add it to the engine:

```python
class Greeter:
    """A plugin that greets once the engine starts."""

    def __init__(self, tx):
        self.tx = tx  # the packet handle: what the plugin sends tasks and notifications with

    def start(self):
        # The entry point: runs once, on one of Thryve's workers, when the engine starts.
        thryve.send_notification(self.tx, "Greeting", "Printer", "hello from Greeter")
        return "Started", "Greeter is up", "Main"

    def clean_up(self):
        # Runs once, when the engine stops.
        print("Greeter is done")


config = thryve.config_from_file(HERE / "thryve.toml", root_dir=HERE / "out")
ctx = thryve.ctx_create(config)
tx = thryve.ctx_get_packet_handle(ctx)

greeter = Greeter(tx)
thryve.ctx_add_plugin(ctx, thryve.plugin("Greeter", tx, greeter.start, greeter.clean_up))
```

- **The context** (`ctx`) is what the engine is built from. **The packet handle** (`tx`) is what everything sends with: tasks to the engine, and notifications to observers.
- **`thryve.plugin(plugin_type, tx, start, clean_up=None)`** makes the plugin. Each plugin in an engine needs a type of its own. A second one with the same type stops the engine from starting.
- **`start`, the entry point,** runs once, on one of Thryve's workers, when the engine starts. It returns its result as `(result_type, message, log_file)`. The message goes to that log file: `"Main"` is `Main.log`. `None` reports nothing. A result of type `"Fin"` stops the engine, and `"FinErr"` stops it with an error.
- **`clean_up`**, if you give one, runs once, when the engine stops.
- **`thryve.ctx_add_plugin`** hands the plugin to the context. If it can't, it frees the plugin and raises `thryve.Error`.

If `start` raises, its traceback is printed, and its message goes to `Error.log`, such as `OSError: disk full`. If `clean_up` raises, its traceback is printed, and `Error.log` says the plugin failed to clean up.

---

## Writing Your First Observer

An observer receives notifications. It runs on a thread of its own, and its job is its whole loop:

```python
class Printer:
    """An observer that prints what it's sent."""

    def run(self, notifications):
        # The observer's whole loop: runs once, on the observer's own thread. Each notification's
        # data comes as it's sent; the loop ends when the engine stops.
        for data in notifications:
            print(data)


thryve.ctx_add_observer(ctx, thryve.observer("Printer", tx, Printer().run))
```

- **`thryve.observer(observer_type, tx, run, clean_up=None)`** makes the observer.
- **`run`** is called once, with the data of the notifications sent to the observer, as they come. The loop ends when the engine stops, and the observer's thread with it.
- **Notifications** are sent with `thryve.send_notification(tx, notif_type, receiver, data)`. `receiver` is the observer's type: Greeter's notification above reaches Printer by its type, `"Printer"`, and neither uses the other's code.
- **`data`** can be any Python object. It's passed as it is, not copied or pickled, so the sender mustn't change it once it's sent.
- **`clean_up`**, if you give one, runs when the engine stops, before the loop ends. Code after the loop mustn't use what `clean_up` let go.

An exception that escapes the loop prints its traceback and ends the loop: the observer receives nothing more. To keep the observer going, catch exceptions inside the loop.

---

## Stopping the Engine

There's no stop call. The engine runs until a task's result has the type `"Fin"`, or `"FinErr"`. Whoever knows the work is done sends that task; in the [examples](../../../examples/python), it's the observer that prints the last report:

```python
thryve.send_task(tx, "Shutdown", 0, lambda: ("Fin", "the work is done", "Main"))
```

`thryve.run_interactive(ctx)` starts the engine, and waits until it stops. It uses up the context. Send the Fin task when the work is done. The engine gives running tasks `shutdown_grace_period` seconds to finish, and `Error.log` counts anything left undone.

To decide in your main program when to stop, such as when someone presses Enter, start the engine and wait for it in two steps:

```python
run = thryve.lib.thryve_run_interactive_start(ctx)  # uses up the context
if not run:
    raise thryve.Error(thryve.last_error())

try:
    input("Press Enter to stop\n")
except (KeyboardInterrupt, EOFError):
    pass

thryve.send_task(tx, "Shutdown", 0, lambda: ("Fin", "stopped from the keyboard", "Main"))
if thryve.lib.thryve_run_interactive_free(run) != thryve.OK:  # waits until the engine stops
    raise thryve.Error(thryve.last_error())
```

`run_interactive` is these two calls, with nothing in between. While it waits, Python doesn't see Ctrl+C until the engine stops. The two steps above wait in Python instead, so Ctrl+C ends the `input`.

---

## Writing Typed Tasks

A typed task's job returns a value or raises an error. The engine hands that result to whoever follows the task, so tasks never need to know about each other: Read doesn't know Hash follows it, and Hash only knows it gets some bytes. The [FFI README](../README.md#typed-tasks) has the whole model; this section shows it from Python.

Tasks are sent while the engine runs: from a plugin's entry point, from a job, or from an observer.

### A value, then a follow-up

```python
import hashlib
from pathlib import Path

# Read: the file's bytes are the task's value.
read = thryve.send_adv_task(tx, "Read", 2, lambda: Path("photo.png").read_bytes())


def hash_bytes(handoff):
    """Hash: follows Read. It gets Read's bytes, or Read's error, which it passes on."""
    if handoff.error:
        raise handoff.error
    return hashlib.sha256(handoff.value).hexdigest()


digest = thryve.then(read, "Hash", hash_bytes)
```

`thryve.then` declares the follow-up at once. It waits in the engine until Read is done, so it can be declared before or after Read runs. It uses up Read's handle, since a task has one follow-up, and returns Hash's own handle, so steps can be chained.

A follow-up's job gets a `Handoff`:

- `handoff.value`: the task's value.
- `handoff.values`: every value, in order. A follow-up has one, and a join has one per task.
- `handoff.error`: the error, as a `thryve.TaskError`, or `None`.

Read the handoff while the job runs. The values themselves are ordinary Python objects: the job can keep them, or return one as its own value.

Every handle has to end somewhere: in a call that uses it up, or in `thryve.free_handle(handle)`, which says nothing follows the task. The task still runs. Then its value is let go, or its error written to `Error.log`.

### Errors

A job fails by raising an exception. `thryve.TaskError(code, message)` sets the error's code; any other exception fails with code 1, and its message:

```python
def read_photo():
    path = Path("photo.png")
    if not path.exists():
        raise thryve.TaskError(404, f"{path} isn't there")
    return path.read_bytes()
```

- **What a follow-up reads:** `handoff.error` has `code` and `message`. In a join, it also has `position`: which task failed, counted from 0.
- **Passing it on:** `raise handoff.error` passes the error on as it is, code included.
- **An error nobody follows** goes to `Error.log`, with the task's name, the message and the code:

  ```
  task 'Report' returned an error: [Errno 2] No such file or directory: 'photo.png' (code 1)
  ```

### Several followers, joined

```python
import zlib


def compress(handoff):
    """Compress: how small Read's bytes get, compressed."""
    if handoff.error:
        raise handoff.error
    return len(zlib.compress(handoff.value))


def report(handoff):
    """Report: joins Hash and Compress."""
    if handoff.error:
        print(f"step {handoff.error.position} failed: {handoff.error.message}")
        raise handoff.error
    digest, size = handoff.values
    print(f"sha256 {digest}, {size} bytes compressed")


# Hash and Compress share Read's bytes: a fan-out.
read = thryve.send_adv_task(tx, "Read", 2, lambda: Path("photo.png").read_bytes())
group = thryve.fan_out(read)
steps = [
    thryve.fanout_then(group, "Hash", hash_bytes),
    thryve.fanout_then(group, "Compress", compress),
]
thryve.fanout_send(group)

# Report: once both are done, with their values in this order, or as soon as one fails.
thryve.free_handle(thryve.join(steps, "Report", report))
```

- **The fan-out** lets several followers share one task's value. Hash and Compress read the same bytes at the same time, so neither may change them. `fanout_send` sends the followers, and uses the group up.
- **`thryve.join`** gives several tasks one follow-up. Report runs once all of them are done, with their values in the list's order, or as soon as one fails, with that one's error.

### Splitting work

```python
paths = sorted(Path("photos").glob("*.png"))


def hash_some(first, count):
    """HashFiles: hashes one piece of the list: `count` paths from `first`."""
    return [hashlib.sha256(path.read_bytes()).hexdigest() for path in paths[first : first + count]]


def collect(handoff):
    """Collect: the pieces' digests, in the list's order."""
    if handoff.error:
        raise handoff.error
    return [digest for piece in handoff.values for digest in piece]


pieces = thryve.split_into(tx, len(paths), "HashFiles", 4, 2, hash_some)
digests = thryve.join(pieces, "Collect", collect)  # Collect's handle, for whatever follows it
```

`thryve.split_into(tx, item_count, name, pieces, priority, job)` cuts `item_count` items into at most `pieces` pieces, in order, and sends one task per piece.

- **The cut is balanced:** sizes differ by one item at most, and no piece is empty.
- **The items stay where they are:** each piece's job gets where its items start and how many it has, and reads them from a list it shares with the other pieces.
- **It returns the pieces' handles:** fewer than `pieces` when there are fewer items, and none when there are no items. A join of no handles fails, so check for an empty list first.

[`examples/python/log_report.py`](../../../examples/python/log_report.py) runs a whole pipeline: a split, a join, a fan-out, and another join.

### Rules to keep in mind

- **Handles:** every call that takes a handle uses it up, even one that fails. Don't use a handle twice.
- **Shared values:** a fan-out's followers share one value, so none may change it.
- **Threads:** jobs run on Thryve's workers, several at a time. See [Threads and the GIL](#threads-and-the-gil).
- **Exceptions:** an exception never leaves a job. `thryve.py` turns it into the task's error.
- **Priorities** go from 0 to `max_priority` (4 by default, set in the configuration). When tasks wait for a worker, the higher priority goes first. A waiting task's priority rises over time, so no task waits forever. A follow-up, a join or a fan-out follower takes its priority from the tasks it follows.

---

## Common Patterns

### A plain task

```python
def save():
    Path("report.txt").write_text("...")
    return "Saved", "report.txt saved", "Main"


thryve.send_task(tx, "Save", 2, save)
```

A plain task's job returns its result the way a plugin's entry point does: `(result_type, message, log_file)`, or `None`. It has no value to hand on; for that, use a typed task.

### Sending a notification

```python
thryve.send_notification(tx, "Progress", "Printer", {"done": 3, "of": 8})
```

### Finding out why a call failed

Every call in `thryve.py` that can fail raises `thryve.Error`, with Thryve's reason:

```python
try:
    thryve.send_task(tx, "", 2, save)
except thryve.Error as error:
    print(error)  # task name must not be empty
```

Sending fails before the engine starts and once it has stopped: `engine is not running: failed to send task to the engine`.

### Log files of your own

A result can go to a log file of your own, once the context knows its name:

```python
import ctypes

names = [b"Audit"]
array = (ctypes.c_char_p * len(names))(*names)
if thryve.lib.thryve_ctx_set_logging_file_names(ctx, array, len(names)) != thryve.OK:
    raise thryve.Error(thryve.last_error())
```

A job that returns `("Saved", "report.txt saved", "Audit")` then writes to `Audit.log`.

- **The names to avoid:** `Main`, `Error` and `Debug` are Thryve's own. A name that's one of them, or one given twice (letter case doesn't count), stops the engine from starting.
- **A file that isn't registered:** a message for it goes to `Error.log`, with a note.

### A complete program

Hasher, a plugin, hashes a file on Thryve's workers. Printer, an observer, prints the digest and ends the run.

```python
"""Hashes a file on Thryve's workers, and prints its digest."""

import hashlib
import sys
from pathlib import Path

import thryve

HERE = Path(__file__).resolve().parent


class Hasher:
    """A plugin: once the engine starts, it sends Hash, and Send follows it."""

    def __init__(self, tx, path):
        self.tx = tx
        self.path = path

    def start(self):
        # The entry point: runs once, on a worker, when the engine starts.
        digest = thryve.send_adv_task(self.tx, "Hash", 2, self.hash)
        thryve.free_handle(thryve.then(digest, "Send", self.send))  # nothing follows Send
        return "Started", f"hashing {self.path}", "Main"

    def hash(self):
        # Hash: the file's digest is the task's value.
        return hashlib.sha256(self.path.read_bytes()).hexdigest()

    def send(self, handoff):
        # Send: Hash's digest, or Hash's error, goes to Printer as a notification.
        if handoff.error:
            text = f"can't hash {self.path}: {handoff.error.message}"
        else:
            text = f"{self.path.name}: {handoff.value}"
        thryve.send_notification(self.tx, "Digest", "Printer", text)


class Printer:
    """An observer: prints what it's sent, then ends the run."""

    def __init__(self, tx):
        self.tx = tx

    def run(self, notifications):
        # The observer's whole loop: runs once, on its own thread, until the engine stops.
        for text in notifications:
            print(text, flush=True)
            # The digest is all this program waits for: a task with a Fin result ends the run.
            thryve.send_task(self.tx, "Shutdown", 0, lambda: ("Fin", "the digest is out", "Main"))


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "photo.png"
    try:
        thryve.load()
        config = thryve.config_from_file(HERE / "thryve.toml", root_dir=HERE / "out")
        ctx = thryve.ctx_create(config)
        tx = thryve.ctx_get_packet_handle(ctx)

        # Hasher and Printer never name each other: the digest reaches Printer as a notification.
        thryve.ctx_add_plugin(ctx, thryve.plugin("Hasher", tx, Hasher(tx, path).start))
        thryve.ctx_add_observer(ctx, thryve.observer("Printer", tx, Printer(tx).run))

        # Runs until Printer's Fin task stops the engine. Running it uses up the context.
        thryve.run_interactive(ctx)
    except thryve.Error as error:
        print(error, file=sys.stderr)
        return 1

    thryve.lib.thryve_packet_handle_free(tx)
    thryve.lib.thryve_config_free(config)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

`python main.py photo.png` prints the file's digest. For a file that isn't there, it prints why: Hash's error reaches Send, which sends it to Printer.

[`examples/python/`](../../../examples/python) has two bigger programs built the same way: a report on a web server's access log, and two projects in one engine. [`examples/README.md`](../../../examples/README.md) says how to run them.

---

## Threads and the GIL

Thryve calls your Python code from its own threads:

- plugins' entry points and tasks' jobs, on its workers (`worker_count` in the configuration, 4 by default), several at a time;
- each observer's job, on a thread of its own;
- clean-ups, when the engine stops.

**Only one thread runs Python at a time.** Each call into Python takes the GIL, the lock CPython's threads take turns with, as with any Python threads.

- **Work outside Python runs in parallel.** Jobs that spend their time where the GIL is let go run at the same time:
  - reading and writing files;
  - hashing large data with `hashlib`;
  - compressing with `zlib`;
  - many NumPy operations.
- **Pure Python work doesn't get faster with more workers.** It still gets Thryve's ordering: priorities, follow-ups and joins.
- **Calls into Thryve let the GIL go.** `ctypes` does that for every call, so the main thread waiting in `run_interactive` doesn't hold up the jobs.

Jobs run on several threads, so the usual rules for threads apply: guard what several jobs change with a `threading.Lock`. Better still, hand values from task to task, which needs no lock.

Free-threaded builds of Python haven't been tried with Thryve.

---

## What thryve.py Holds for You

In C, you free what Thryve gives back, and give Thryve free functions for the rest. `thryve.py` does most of that for you:

1. **Python objects:** jobs, values and notifications' data are held by `thryve.py`, under an id that Thryve carries in their place.
   - Thryve lets an id go when it's done with it: after the job has run, or once nobody can take the value. `thryve.py` lets the object go then.
   - A notification's data is handed over when its observer receives it. Data that no observer ever receives stays held until the program ends.
2. **Values are passed, never copied:** a follow-up gets the very object the task returned.
3. **Handles** are used up by every call that takes one, even one that fails:
   - `then`, `join` and `fan_out` take handles;
   - `fanout_send` takes its group;
   - a handle that nothing follows ends with `free_handle`.
4. **Plugins and observers** belong to the context once added. If `ctx_add_plugin` or `ctx_add_observer` can't add one, it frees it, and raises `thryve.Error`.
5. **The context** is used up by `run_interactive`, or by `thryve_run_interactive_start`.
6. **Names** are copied: pass any `str`.
7. **Yours to free**, once the engine has stopped: the packet handle and the configuration.

   ```python
   thryve.lib.thryve_packet_handle_free(tx)
   thryve.lib.thryve_config_free(config)
   ```

---

## Beyond thryve.py

`thryve.py` wraps what most programs need. Everything in `thryve.h` is also in `thryve.lib`, under its C name and with its C types:

- handles are integers, or `None` for NULL;
- strings are `bytes`;
- statuses are `thryve.OK` and the other constants at the top of `thryve.py`.

After a call through `thryve.lib` fails, `thryve.last_error()` says why. Read it right away, on the same thread.

| To | Call, in `thryve.lib` |
|----|------------------------|
| Cancel a typed task | `thryve_adv_handle_cancel`, or `thryve_adv_handles_cancel_all` for several |
| Give a follow-up, a join or a fan-out follower a priority of its own | `thryve_adv_handle_then_with_priority`, `thryve_adv_handles_join_with_priority`, `thryve_fanout_then_with_priority` |
| Forward: a job sends another task, and its followers get that task's result | `thryve_adv_task_create_forward`, `thryve_adv_handle_then_forward`, `thryve_outcome_forward` |
| Cut work into chunks of your own | `thryve_packet_handle_send_split_tasks` |
| Run a plain task after another | `thryve_task_result_new_with_chain` |
| Run as a Windows service | `thryve_service_create`, `thryve_service_run` |

Cancelling takes only a handle, so it's one call:

```python
handle = thryve.send_adv_task(tx, "Read", 2, read_photo)
thryve.lib.thryve_adv_handle_cancel(handle)  # uses the handle up
```

A task that hasn't started never runs. A running one finishes, and its result is let go. The [FFI README](../README.md#cancelling) says what else stops with it.

The rest take a job, which has to reach Thryve as a C callback:

- **A priority of its own:** copy `then`, `join` or `fanout_then` from `thryve.py`, and call the `_with_priority` function in its place, with the priority after the name.
- **Forwarding:** the job has to return `thryve_outcome_forward`, and `thryve.py`'s jobs don't.
- **A Windows service:** running Thryve as one from Python hasn't been tried.

---

## Troubleshooting

### `Thryve's C library isn't in …`

`load()` didn't find the library. Pass its path, or its folder, to `thryve.load()`, or set `THRYVE_FFI`. A copy of `thryve.py` outside the repository finds the library only that way.

### `OSError: [WinError 193] %1 is not a valid Win32 application`

The Python is 32-bit, and the library is 64-bit. Use a 64-bit Python. This prints 64 for one:

```bash
python -c "import struct; print(struct.calcsize('P') * 8)"
```

### `AttributeError: 'NoneType' object has no attribute 'thryve_…'`

`thryve.load()` hasn't run yet. Call it first.

### The program never ends

Nothing sent a task with a `Fin` (or `FinErr`) result. `run_interactive` waits until one stops the engine. See [Stopping the Engine](#stopping-the-engine).

### Ctrl+C doesn't stop the program

While `run_interactive` waits, Python's main thread is inside the C library, and Python handles Ctrl+C only once it's back in Python code: after the engine stops. Start and wait in two steps instead, as in [Stopping the Engine](#stopping-the-engine).

### The logs are in Python's folder

Thryve has no root folder, so it uses the program's folder, which for Python is `python.exe`'s. Give it one with `config_from_file(..., root_dir=...)`, as in [Configuration and the Root Folder](#configuration-and-the-root-folder).

### Topology doesn't open in graph mode

Thryve looks for `Topology.exe` next to the program, which for Python means next to `python.exe`, and says on the console when it isn't there. Either:

- put `Topology.exe` there;
- or open the graph yourself, once the run is over:

  ```bash
  Topology.exe <root folder>\topology\topology_export.ron
  ```

### `failed to start up: plugin type "Greeter" cannot be registered more than once`

Two plugins in one engine have the same type. Give each its own.

### `engine is not running: failed to send task to the engine`

Something was sent before the engine started, or after it stopped. Send from plugins' entry points, jobs and observers, or after `thryve_run_interactive_start`.

### Python exits with code 1, and `Error.log` reports a panic

A panic inside Thryve ends the whole process, Python with it. An exception in your Python code never does: `thryve.py` catches it.
