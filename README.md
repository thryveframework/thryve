# Thryve Task Manager

A high-performance, extensible task management and execution framework written in Rust. Thryve provides a robust architecture for managing concurrent task execution, event-driven notifications, and modular plugin-based extensions.

What changed in each release is in [CHANGELOG.md](CHANGELOG.md). Runnable examples, the same program in Rust, C, C++ and Python, are in [examples/](examples/).

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Core Concepts](#core-concepts)
- [Modules](#modules)
- [Configuration](#configuration)
- [Usage](#usage)
- [Graph Visualization](#graph-visualization-debug-feature)
- [Building](#building)
- [Log Output Format](#log-output-format)
- [License](#license)
- [Contributing](#contributing)

## Overview

Thryve is designed around three main pillars:

1. **Task Execution**: Submit tasks with priorities to a thread pool for concurrent execution
2. **Observer Pattern**: Event-driven notification system for decoupled component communication
3. **Plugin System**: Extensible architecture allowing dynamic functionality addition

### Key Features

- **Priority-based scheduling** with aging mechanism to prevent task starvation
- **Typed tasks with follow-ups, splits, joins and fan-out**: a task's value, or its own error, goes to the task declared after it with `then`; work splits over several tasks with `split_into`, several tasks share one follow-up with `join`, and several follow-ups share one result with `fan_out`, so plugins combine without knowing each other
- **Cancelling**: `cancel` on a typed task's handle keeps a task that hasn't started from running, and the work that only it needed stops too: the tasks a join still waits for, the tasks a follow-up's chain was waiting on, and the task a fan-out group follows once every follower is cancelled
- **Graceful shutdown** with pause/resume capabilities
- **Thread-safe** concurrent data structures using `crossbeam`
- **Configurable** via TOML with sensible defaults
- **Extensible** through plugins and observers
- **Production-ready** logging with timestamped, categorized output
- **Graph visualization** for task topology analysis (debug mode)

## Architecture

![Architecture](./md_images/Architecture.png)

### Data Flow

1. **Sending**: your program, plugins and observers send tasks and notifications through a `PacketHandle`, which refuses them until the engine exists: start Thryve, then send
2. **Mailboxes**: everything lands in one of four mailboxes that only the engine reads, in this order each turn: status messages, tasks, notifications and results. Status messages are Thryve's own (status lines for the logger, the releases of dropped task handles, cancels, and fan-out groups' data), and their mailbox never makes a sender wait
3. **Tasks** are queued by priority, with aging, for the worker threads; a typed task's follow-up, join or fan-out follower waits in the engine's wait tables until its inputs arrive
4. **Worker threads** run the tasks, skip the ones that were cancelled, and report each result to the engine, which passes a typed task's result on to its follow-up, into its join, or to a fan-out group's followers
5. **Notifications** go to the observers: a sorter thread routes each to its observer's queue, and each observer runs on a thread of its own
6. **Shutdown**: a `Fin` task ends the run; in graph mode, the topology handler then exports the graph and opens it in Topology

## Core Concepts

### Tasks

Tasks are units of work submitted to the engine for execution:

```rust
use thryve_utils::packets::task::Task;
use thryve_utils::res::TaskResult;

let task = Task::new("MyTask", 2, || {
    // Task execution logic
    TaskResult::new("Success", "Task completed".to_string(), "Main")
}).expect("valid task name");
```

**Task Properties:**
- **Unique ID**: Automatically assigned for tracking
- **Name**: Human-readable identifier
- **Priority**: Higher values = more urgent (0-14)
- **Execution Function**: Closure returning a `TaskResult`

### Results

Results represent task execution outcomes and enable task chaining:

```rust
use thryve_utils::res::TaskResult;
use thryve_utils::packets::task::Task;

// Terminal result
let result = TaskResult::new("Success", "Done".to_string(), "Main");

// Chained result — executes the next task once this one completes
let next_task = Task::new("NextTask", 1, || {
    TaskResult::new("Ok", "".to_string(), "Main")
}).expect("valid task name");
let chained = TaskResult::new("Chained", "Moving on".to_string(), "Main").chain(next_task);
```

### Typed Tasks and Follow-ups

A typed task, `AdvTask<T, E>`, returns `Result<T, E>`: a value, or its own error type. Sending it returns a handle, and `then` on the handle declares a follow-up: a task of its own that runs once the first one finishes, and gets its value or its error as a `Handoff`:

```rust
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::packets::task::AdvTask;
use thryve_utils::res::Handoff;

fn size_in_kb(handle: &PacketHandle) -> Result<(), Box<dyn std::error::Error>> {
    let file_size = AdvTask::new("FileSize", 2, || {
        std::fs::metadata("photo.png").map(|meta| meta.len())
    })?;

    handle
        .send_adv_task(file_size)?
        .then("SizeInKb", |handoff| match handoff {
            Handoff::Value(bytes) => Ok(bytes / 1024),
            // `FileSize`'s own error, a `std::io::Error`, passed on to whoever follows.
            Handoff::Error(error) => Err(error),
        })?;

    Ok(())
}
```

- **Chains:** `then` returns the follow-up's handle, so steps chain. A task has one follow-up: `then` uses its handle up. To give one result to several follow-ups, see [Fan-out](#fan-out).
- **Timing:** `then` works before or after the task finishes, since the engine keeps the result while the handle exists. Dropping a handle without `then` lets the result go, and an error is written to `Error.log`.
- **Priority:** a follow-up inherits its task's priority, capped below realtime. `then_with_priority` gives it its own.
- **Forwarding:** a task that creates its last step while it runs can forward to it with `AdvTask::forward` (or `then_forward`). It returns that step's handle, and whoever follows the task gets the step's result.
- **Independent plugins:** a plugin exports functions that send its typed tasks and return their handles. The code that uses two plugins wires them together with `then`, so neither plugin names the other.

Plain `Task`s and `chain` keep working for fire-and-forget work.

### Joins

Several typed tasks of one type can share one follow-up: `join`, called on a `Vec` of their handles, uses the handles up, and its closure gets a `JoinHandoff`: every value, in the order of the handles, or the first error and the position of the task that failed. `join` comes from the `Joinable` trait, so import it; without it, `.join(…)` on a `Vec` finds Rust's own `join`, which joins strings:

```rust
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::packets::task::AdvTask;
use thryve_utils::res::JoinHandoff;
use thryve_utils::traits::join::Joinable;

fn total_size(handle: &PacketHandle) -> Result<(), Box<dyn std::error::Error>> {
    let mut sizes = Vec::new();
    for path in ["a.png", "b.png", "c.png"] {
        let file_size = AdvTask::new("FileSize", 2, move || {
            std::fs::metadata(path).map(|meta| meta.len())
        })?;
        sizes.push(handle.send_adv_task(file_size)?);
    }

    sizes.join("TotalSize", |joined| match joined {
        // Every file's size, in the order of `sizes`.
        JoinHandoff::Filled { values } => Ok(values.iter().sum::<u64>()),
        // The first `FileSize` that failed, with its own `std::io::Error`.
        JoinHandoff::Error { error, .. } => Err(error),
    })?;

    Ok(())
}
```

- **Fails fast:** the join runs as soon as one task fails, and the tasks it was still waiting for are cancelled (see [Cancelling](#cancelling)): those that haven't started by then never run, and those already running finish with nobody to take their results, so an error is written to `Error.log`.
- **A follow-up like any other:** `join` returns the join's own handle, so the join can be followed with `then`, or be one of the tasks of another join. It waits for each task whether that task finished before the join was declared or after, and a task that forwards fills its slot with the result of the task it forwarded to.
- **Priority:** the join inherits the highest priority among its tasks, capped below realtime. `join_with_priority` gives it its own.
- **Splitting has a cost:** joins of joins let work be split, and split again: ten images, each split into rows, and the rows' results archived into one zip. But every piece is a task of its own, with its own trip through the queue, its own result and its own slot, and every join adds one more task. Splitting finer than the work needs adds that overhead without making anything faster, so aim for a balance: enough pieces to keep the workers busy, each big enough to be worth a task.

### Fan-out

Several follow-ups can share one task's result: `fan_out` on a typed task's handle uses the handle up and returns a group, `then` on the group declares each follower, and `send_fanout` sends them. Every follower gets the same result, shared rather than copied: an `Arc<Handoff<T, Arc<E>>>`, holding the task's value or its own error:

```rust
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::packets::task::AdvTask;
use thryve_utils::res::Handoff;

fn photo_opened(handle: &PacketHandle) -> Result<(), Box<dyn std::error::Error>> {
    let load = AdvTask::new("LoadImg", 2, || std::fs::read("photo.png"))?;

    let mut group = handle.send_adv_task(load)?.fan_out();
    group.then("Thumbnail", |pixels| match &*pixels {
        Handoff::Value(bytes) => Ok(bytes.len() / 4),
        // LoadImg's own error, passed on: one more count on its `Arc`, not a copy.
        Handoff::Error(error) => Err(error.clone()),
    })?;
    group.then("Compress", |pixels| match &*pixels {
        Handoff::Value(bytes) => Ok(bytes.iter().map(|byte| u64::from(*byte)).sum::<u64>()),
        Handoff::Error(error) => Err(error.clone()),
    })?;
    group.send_fanout()?;

    Ok(())
}
```

- **One result, read in place:** every follower reads the same result, and the last one done with it frees it. Only fan-out asks for the value and the error to be `Sync`, since followers can read them at the same time, on different workers.
- **Ordinary handles:** the group's `then` returns each follower's handle, so a follower can be followed with `then`, forwarded, cancelled, or joined with followers of its own type. A follower inherits the task's priority, capped below realtime; `then_with_priority` gives it its own.
- **Sent even if dropped:** a group dropped without `send_fanout` is sent all the same, and a group with no follower is refused.
- **One error, written once at most:** if the task fails, every follower gets its error. Once any follower takes it, nothing is written to `Error.log`; if every follower was cancelled, it's written once.
- **Cancelling a follower** cancels only that follower: the task still runs for the others, and it's cancelled only once every follower is, if it hasn't started by then (see [Cancelling](#cancelling)).

### Splitting Work

`split_into` on the `PacketHandle` cuts a list into pieces and sends one typed task per piece, all with the same name, priority and closure. It returns their handles in order, ready to be joined:

```rust
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::res::JoinHandoff;
use thryve_utils::traits::join::Joinable;

fn total(handle: &PacketHandle) -> Result<(), Box<dyn std::error::Error>> {
    let numbers: Vec<u64> = (1..=1_000).collect();

    // Four pieces of 250 numbers, each adding up its own; then one total.
    handle
        .split_into(numbers, "PartialSum", 4, 2, |chunk, _start| {
            Ok::<u64, std::io::Error>(chunk.iter().sum())
        })?
        .join("TotalSum", |joined| match joined {
            JoinHandoff::Filled { values } => Ok(values.iter().sum::<u64>()),
            JoinHandoff::Error { error, .. } => Err(error),
        })?;

    Ok(())
}
```

- **A balanced cut:** the pieces' sizes differ by at most one item, and no piece is empty: more pieces than items gives one piece per item. The items are moved into their pieces, never copied, and never padded.
- **The closure** gets its chunk and where that chunk starts in the list, so a piece knows which items it holds. Since sizes can differ by one, it must handle any chunk size; if the work needs items in groups (rows in pairs), group them first, so that each group is one item.
- **Your own cut:** `send_split_tasks` takes chunks you already cut, of any type (by size, at content boundaries, one file per piece), and its closure gets the piece's index.
- **The count is yours,** and splitting has the cost described under Joins.

### Cancelling

`cancel` on a typed task's handle cancels the task and uses the handle up; `cancel_all`, from the `Joinable` trait, cancels every task in a `Vec` of handles, such as the pieces `split_into` gave back:

```rust
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::packets::task::AdvTask;
use thryve_utils::traits::join::Joinable;

fn photo_closed(handle: &PacketHandle) -> Result<(), Box<dyn std::error::Error>> {
    let thumbnail = AdvTask::new("Thumbnail", 2, || Ok::<u64, std::io::Error>(64))?;
    let thumbnail = handle.send_adv_task(thumbnail)?;

    let rows: Vec<u32> = (0..1_000).collect();
    let pieces = handle.split_into(rows, "Row", 8, 2, |chunk, _start| {
        Ok::<u32, std::io::Error>(chunk.iter().sum())
    })?;

    // The photo was closed: neither the thumbnail nor the rows are needed any more.
    thumbnail.cancel();
    pieces.cancel_all();

    Ok(())
}
```

- **Before it starts:** a task that hasn't started never runs: the worker that takes it skips it. A follow-up still waiting for its task, and a join still waiting for its tasks, never run either.
- **Too late:** a task that's already running can't be stopped. It finishes, and its result is let go: an error is still written to `Error.log`, and a value is dropped. The same goes for a task that has already finished.
- **Through forwards and joins:** if the task forwarded, the cancel goes on to the task it forwarded to. Cancelling a join that's still waiting also cancels the tasks it waits for, and the joins among them, all the way in.
- **Up the chain:** the work that only the cancelled task needed stops too. Cancelling a follow-up also cancels the task it follows, since `then` used that task's handle up and nobody else can take its result, and so on up: if Save follows LoadImg, which follows Hash, cancelling Save cancels LoadImg and Hash too, as long as they haven't started. A fan-out follower only withdraws from its group: the task the group follows is cancelled once every follower is.
- **No error is lost:** if a follow-up or a join had already been given an error by the task before it, that error is still written to `Error.log`, since nobody takes it now.
- **Quiet:** cancelling never fails and writes nothing to the logs, even once the engine is gone. At shutdown, a cancelled task isn't counted as lost work.

### Observers

Observers are long-running components that process notifications:

```rust
use thryve_observers::logger::LoggerObserver;
use thryve_utils::cfg::config::Config;
use thryve_run::run::run_interactive::summon_requirements;

// Thryve starts its own logger; this only shows the shape. The logger writes to the log files
// that startup opens, so before startup this returns an error.
let ctx = summon_requirements(Config::load_def_config());
let logger = LoggerObserver::new(&ctx);
```

Your own observers follow the pattern in [Creating an Observer](#creating-an-observer).

### Plugins

Plugins extend functionality without modifying the core:

```rust
use thryve_utils::errors::ThryveError;
use thryve_utils::traits::plugin_logic::PlgLogic;
use thryve_utils::impl_as_any;

// Implement PlgLogic for your plugin
struct MyPlugin;

impl PlgLogic for MyPlugin {
    fn clean_up(&self) -> Result<(), ThryveError> {
        // Cleanup logic
        Ok(())
    }

    impl_as_any!(MyPlugin);
}
```

## Modules

### thryve_core

The central runtime engine coordinating all components.

| Component | Description |
|-----------|-------------|
| `Engine` | Main execution loop, coordinates plugin and observer managers |
| `PluginManager` | Manages plugins, uses priority queue for task scheduling |
| `ObserverManager` | Manages observers, routes notifications via sorter thread |

### thryve_utils

Core utilities, data structures, and traits.

| Component | Description |
|-----------|-------------|
| `Task` | Unit of work with priority and unique ID |
| `AdvTask` / `AdvTaskHandle` | Typed task returning `Result<T, E>`, and the handle its follow-ups are declared on (`then`, `then_with_priority`, `then_forward`, `fan_out`) and that cancels it (`cancel`) |
| `FanoutGroup` | What `fan_out` returns: several followers declared on one task's result with `then`, which share it, sent with `send_fanout` |
| `PacketHandle` | Sends tasks and notifications to the engine, and splits work over typed tasks (`split_into`, `send_split_tasks`) |
| `Joinable` | Trait on `Vec<AdvTaskHandle<T, E>>`: `join` and `join_with_priority` declare one follow-up for several typed tasks of one type, which runs with all their values, or as soon as one fails; `cancel_all` cancels them all |
| `Notif` | Notification structure for observer communication |
| `Log` | Logging notification implementation |
| `TaskResult` | Task execution outcome with chaining capability |
| `Handoff` | What a follow-up gets: the task's value, or its own error |
| `JoinHandoff` | What a join's follow-up gets: every task's value, in order, or the first error and the position of the task that failed |
| `ThreadPool` | Generic thread pool for tasks and notifications |
| `PQChild` | Priority queue with aging mechanism |
| `QChild` | Segmented queue for notifications |
| `Plugin` | Plugin structure |
| `Observer` | Observer structure |
| `Config` | Configuration structures |
| `Ctx` | Application context |

### thryve_observers

Built-in observer implementations.

| Component | Description |
|-----------|-------------|
| `LoggerObserver` | File-based logging with timestamped output |
| `TopologyHandlerObserver` | Records the task graph in graph mode, and exports it at shutdown |

### thryve_run

Service initialization and execution.

| Component | Description |
|-----------|-------------|
| `run_service` | Runs Thryve as a Windows service: `create_service`, then `try_run_as_service` |
| `run_interactive` | Runs Thryve in the foreground: `summon_requirements`, then `run_interactive` |
| `service::Thryve` | The Windows service itself, driven by the service manager's Stop, Pause and Resume |
| `init` | Startup: the log folders (`init_directories`), the built-in observers, and the engine's thread |

### thryve

The crate applications depend on: it re-exports the public parts of the crates above, with a `prelude` of what nearly every plugin needs.

### thryve_ffi

C and C++ bindings, built as a library with a generated header. See [`crates/thryve_ffi/README.md`](crates/thryve_ffi/README.md).

## Configuration

Configuration is loaded from a TOML file with `Config::load_config_from_file`, or the defaults are used with `Config::load_def_config()`.

A config file must set every key shown below. A missing or unknown key is rejected with its line, and so are values that would break the engine: `idle_wait`, `shutdown_grace_period`, `worker_count`, `aging_interval`, and the three pipe capacities and per-turn limits must be at least 1, and `max_priority` must be 14 or lower. `root_dir` must be empty, or an absolute path to a folder that already exists; write a Windows path in single quotes, as in `root_dir = 'D:\ThryveData'`, since inside double quotes TOML reads each backslash as the start of an escape. All invalid values are reported in one error. Loading never falls back to the defaults on its own; that's the caller's choice.

### Default Configuration

This complete file holds the defaults:

```toml
[init]
root_dir = ""

[service]
can_stop = true
can_pause = true

[engine]
idle_wait = 100
shutdown_grace_period = 10

[threads]
worker_count = 4

[pipes]
task_pipe_capacity = 1024
tasks_processed_per_turn = 500
notif_pipe_capacity = 1024
notifs_processed_per_turn = 500
res_pipe_capacity = 1024
res_processed_per_turn = 500

[tasks]
max_priority = 4
aging_interval = 3

[debugging]
is_debugging = false
wait_before_execution = 10
visual_graph= false
calculate_metrics= false
log_tasks= false
```

### Configuration Options

| Section | Option | Type | Default | Description |
|---------|--------|------|---------|-------------|
| `init` | `root_dir` | string | `""` | The root folder, which holds each run's logs and, in graph mode, the task graph. Empty means the program's own folder; anything else must be an absolute path to a folder that already exists, such as `D:\ThryveData` |
| `service` | `can_stop` | bool | true | Accept stop commands |
| `service` | `can_pause` | bool | true | Accept pause/resume commands |
| `engine` | `idle_wait` | u64 | 100 | How long (ms) an idle engine waits for work before it checks its loop's flags (the service's Stop, Pause, Resume) again; anything that arrives wakes it at once |
| `engine` | `shutdown_grace_period` | usize | 10 | Seconds the shutdown waits for running tasks; a task still running then is named in `Error.log` and left behind, and the plugins aren't cleaned up |
| `threads` | `worker_count` | usize | 4 | Number of worker threads |
| `pipes` | `task_pipe_capacity` | usize | 1024 | Task channel buffer size |
| `pipes` | `tasks_processed_per_turn` | usize | 500 | Most tasks the engine takes per turn of its loop; the rest wait for the next turn |
| `pipes` | `notif_pipe_capacity` | usize | 1024 | Notification channel buffer size |
| `pipes` | `notifs_processed_per_turn` | usize | 500 | Most notifications the engine takes per turn |
| `pipes` | `res_pipe_capacity` | usize | 1024 | Result channel buffer size |
| `pipes` | `res_processed_per_turn` | usize | 500 | Most results the engine takes per turn |
| `tasks` | `max_priority` | u8 | 4 | Maximum task priority (0-14) |
| `tasks` | `aging_interval` | u64 | 3 | Seconds a task waits for each one-level priority boost |
| `debugging` | `is_debugging` | bool | false | Enable debug mode |
| `debugging` | `wait_before_execution` | u64 | 10 | Debug wait time (seconds) |
| `debugging` | `visual_graph` | bool | false | Enable task topology graph visualization |
| `debugging` | `calculate_metrics` | bool | false | Enable metrics calculation |
| `debugging` | `log_tasks` | bool | false | Enable task execution logging |

## Usage

### Creating a Simple Task

```rust
use thryve_utils::packets::task::Task;
use thryve_utils::res::TaskResult;
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::errors::ThryveError;

fn send_calculation(handle: &PacketHandle) -> Result<(), ThryveError> {
    let task = Task::new("CalculateSum", 1, || {
        let result = 2 + 2;
        TaskResult::new("Success", format!("Result: {}", result), "Main")
    })?;

    // Fails only once the engine is gone; the task is then dropped.
    handle.send_task(task)
}
```

### Creating a Plugin

The idiomatic pattern is to give your plugin's logic type its own `new` that builds and returns the whole `Plugin` — rather than a separate free function:

```rust
use std::sync::Arc;
use thryve_utils::bases::plugin::Plugin;
use thryve_utils::errors::ThryveError;
use thryve_utils::traits::plugin_logic::PlgLogic;
use thryve_utils::impl_as_any;
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::res::TaskResult;

struct MyPlugin;

impl MyPlugin {
    pub fn new(tx: PacketHandle) -> Plugin {
        let core = Arc::new(Self);
        let core_clone = Arc::clone(&core);
        let tx_clone = tx.clone();

        // The task job calls back into the plugin's own logic, so it has
        // access to whatever state the plugin needs.
        let entry_point = move || core_clone.task_a(tx_clone);

        Plugin::new("MyPluginType", core, tx, entry_point).expect("valid plugin type")
    }

    fn task_a(&self, _tx: PacketHandle) -> TaskResult {
        TaskResult::new("Success", "Task A completed".to_string(), "Main")
    }
}

impl PlgLogic for MyPlugin {
    fn clean_up(&self) -> Result<(), ThryveError> {
        println!("Cleaning up plugin");
        Ok(())
    }

    impl_as_any!(MyPlugin);
}
```

### Creating an Observer

Observers follow the same self-constructing pattern as plugins:

```rust
use std::sync::Arc;
use thryve_utils::bases::observer::Observer;
use thryve_utils::traits::observer_logic::ObsLogic;
use thryve_utils::traits::queue::Queue;
use thryve_utils::extras::jobs::ObsJob;
use thryve_utils::impl_as_any;
use thryve_utils::packets::packet_handle::PacketHandle;

struct MyObserver;

impl MyObserver {
    pub fn new(tx: PacketHandle) -> Observer {
        let core = Arc::new(Self);

        let run_fn = ObsJob::new(move |observer: Arc<Observer>| {
            loop {
                if observer.exit_flag.load(std::sync::atomic::Ordering::Relaxed) {
                    return;
                }

                // `pop` waits for the next notification. It returns `None` once the
                // queue is finalized at shutdown.
                let Some(_notif) = observer.obs_queue.pop() else {
                    return;
                };

                // Handle the notification.
            }
        });

        Observer::new("MyObserverType", core, run_fn, tx).expect("valid observer type")
    }
}

impl ObsLogic for MyObserver {
    fn clean_up(&self, _observer: Arc<Observer>) {
        println!("Cleaning up observer");
    }

    impl_as_any!(MyObserver);
}
```

### Full Sample Service

These restate the same `MyPlugin`/`MyObserver` shape from [Creating a Plugin](#creating-a-plugin) and [Creating an Observer](#creating-an-observer) so the sample is fully self-contained. Marked `no_run` since actually executing it attempts to install and run a real Windows service.

```rust,no_run
use std::sync::Arc;
use thryve_run::run::run_service;
use thryve_utils::cfg::config::Config;
use thryve_utils::bases::plugin::Plugin;
use thryve_utils::bases::observer::Observer;
use thryve_utils::traits::plugin_logic::PlgLogic;
use thryve_utils::traits::observer_logic::ObsLogic;
use thryve_utils::traits::queue::Queue;
use thryve_utils::errors::{ThryveError, get_err};
use thryve_utils::extras::jobs::ObsJob;
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::res::TaskResult;
use thryve_utils::impl_as_any;

struct MyPlugin;
impl MyPlugin {
    pub fn new(tx: PacketHandle) -> Plugin {
        let core = Arc::new(Self);
        let core_clone = Arc::clone(&core);
        let tx_clone = tx.clone();
        let entry_point = move || core_clone.task_a(tx_clone);
        Plugin::new("MyPluginType", core, tx, entry_point).expect("valid plugin type")
    }
    fn task_a(&self, _tx: PacketHandle) -> TaskResult {
        TaskResult::new("Success", "Task A completed".to_string(), "Main")
    }
}
impl PlgLogic for MyPlugin {
    fn clean_up(&self) -> Result<(), ThryveError> {
        Ok(())
    }
    impl_as_any!(MyPlugin);
}

struct MyObserver;
impl MyObserver {
    pub fn new(tx: PacketHandle) -> Observer {
        let core = Arc::new(Self);
        let run_fn = ObsJob::new(move |observer: Arc<Observer>| {
            loop {
                if observer.exit_flag.load(std::sync::atomic::Ordering::Relaxed) {
                    return;
                }

                // `pop` waits for the next notification. It returns `None` once the
                // queue is finalized at shutdown.
                let Some(_notif) = observer.obs_queue.pop() else {
                    return;
                };

                // Handle the notification.
            }
        });
        Observer::new("MyObserverType", core, run_fn, tx).expect("valid observer type")
    }
}
impl ObsLogic for MyObserver {
    fn clean_up(&self, _observer: Arc<Observer>) {}
    impl_as_any!(MyObserver);
}

fn main() {
    let config = Config::load_def_config();
    let (svc, mut ctx) = run_service::create_service(config);

    let plugin = MyPlugin::new(ctx.packet_handle());
    let observer = MyObserver::new(ctx.packet_handle());

    ctx.plugins.push(plugin);
    ctx.observers.push(observer);

    // A TestFile is added to the logging files.
    ctx.logging_file_names.push("TestFile".to_string());

    // A failed start has already stopped every thread it started, and its error is in
    // Error.log once the log folder exists. Under the service manager stderr goes nowhere,
    // but the exit code is recorded.
    if let Err(e) = run_service::try_run_as_service(svc, ctx) {
        eprintln!("{}", get_err(&e));
        std::process::exit(1);
    }
}
```

### Full Sample Interactive

```rust,no_run
use std::sync::Arc;
use thryve_run::run::run_interactive;
use thryve_utils::cfg::config::Config;
use thryve_utils::bases::plugin::Plugin;
use thryve_utils::bases::observer::Observer;
use thryve_utils::traits::plugin_logic::PlgLogic;
use thryve_utils::traits::observer_logic::ObsLogic;
use thryve_utils::traits::queue::Queue;
use thryve_utils::errors::{ThryveError, get_err};
use thryve_utils::extras::jobs::ObsJob;
use thryve_utils::packets::packet_handle::PacketHandle;
use thryve_utils::res::TaskResult;
use thryve_utils::impl_as_any;

struct MyPlugin;
impl MyPlugin {
    pub fn new(tx: PacketHandle) -> Plugin {
        let core = Arc::new(Self);
        let core_clone = Arc::clone(&core);
        let tx_clone = tx.clone();
        let entry_point = move || core_clone.task_a(tx_clone);
        Plugin::new("MyPluginType", core, tx, entry_point).expect("valid plugin type")
    }
    fn task_a(&self, _tx: PacketHandle) -> TaskResult {
        TaskResult::new("Success", "Task A completed".to_string(), "Main")
    }
}
impl PlgLogic for MyPlugin {
    fn clean_up(&self) -> Result<(), ThryveError> {
        Ok(())
    }
    impl_as_any!(MyPlugin);
}

struct MyObserver;
impl MyObserver {
    pub fn new(tx: PacketHandle) -> Observer {
        let core = Arc::new(Self);
        let run_fn = ObsJob::new(move |observer: Arc<Observer>| {
            loop {
                if observer.exit_flag.load(std::sync::atomic::Ordering::Relaxed) {
                    return;
                }

                // `pop` waits for the next notification. It returns `None` once the
                // queue is finalized at shutdown.
                let Some(_notif) = observer.obs_queue.pop() else {
                    return;
                };

                // Handle the notification.
            }
        });
        Observer::new("MyObserverType", core, run_fn, tx).expect("valid observer type")
    }
}
impl ObsLogic for MyObserver {
    fn clean_up(&self, _observer: Arc<Observer>) {}
    impl_as_any!(MyObserver);
}

fn main() {
    let config = Config::load_def_config();
    let mut ctx = run_interactive::summon_requirements(config);

    let plugin = MyPlugin::new(ctx.packet_handle());
    let observer = MyObserver::new(ctx.packet_handle());

    ctx.plugins.push(plugin);
    ctx.observers.push(observer);

    // A TestFile is added to the logging files.
    ctx.logging_file_names.push("TestFile".to_string());

    match run_interactive::run_interactive(ctx) {
        Ok(handle) => {
            let _ = handle.join();
        }
        // A failed start has already stopped every thread it started. Its error is also
        // written to Error.log once the log folder exists.
        Err(e) => eprintln!("{}", get_err(&e)),
    }
}
```

## Graph Visualization (Debug Feature)

When enabled via the `visual_graph` configuration option, Thryve automatically tracks task creation points and builds a topology graph showing how tasks call each other.

### How It Works

1. **Call Site Capture**: When a task is sent (`send_task`) or chained (`chain`), the system records the file path and line number of that call. The compiler passes them in (`#[track_caller]`), so they're exact in every build, with or without debug info. A function of yours that wraps `send_task` is the call site for every task it sends, unless you mark it `#[track_caller]` too.
2. **Fingerprint Generation**: Each call site is assigned a unique fingerprint based on its location
3. **Graph Building**: The [`TopologyHandlerObserver`](crates/thryve_observers/src/topology_handler.rs) receives notifications and builds a graph where:
   - **Nodes** represent call sites where tasks are created
   - **Edges** represent parent-child task relationships
   - **Edge weights** represent how many times a call path was taken
4. **Visualization**: On shutdown, the graph is exported to `topology/topology_export.ron` in the root folder, and Topology (`Topology.exe`) is started with that file's path. Get `Topology.exe` from Thryve's [release page](https://github.com/thryveframework/thryve/releases/latest), on its own or next to the C library, and put it next to your program's `.exe`: Thryve looks for it there, never in the root folder, since whoever can write to the root could put their own program there.

### When Graph Mode Fails

Graph mode is a debugging aid, so a failure in it never stops the engine:

- If the graph would be wrong from some point on, graph mode turns off until the program restarts, with a warning in `Error.log`. That happens when an edge points to a node that isn't in the graph, or the graph's lock was poisoned. The graph recorded so far is still exported. It ends with a *failure node* that says why, linked from the node where the failure happened.
- Smaller problems are handled on the spot and noted in `Error.log`: outside a cargo workspace, file paths stay absolute; a failed export, or a failed start of Topology, is reported, and the shutdown goes on.

### Enabling Graph Visualization

In your configuration file:

```toml
[debugging]
is_debugging = true
visual_graph = true
```

### Architecture

The graph visualization system consists of:

| Component | Description |
|-----------|-------------|
| [`TaskCallSite`](crates/thryve_utils/src/graphs/callsite.rs) | Represents a single call site with file path, line number and task name |
| [`TaskTopology`](crates/thryve_utils/src/graphs/graph.rs) | Maintains the graph structure using petgraph's StableGraph |
| [`TopologyModifier`](crates/thryve_utils/src/packets/notif_topology.rs) | Notification type for adding nodes and edges |
| [`TopologyHandlerObserver`](crates/thryve_observers/src/topology_handler.rs) | Observer that maintains the graph and exports it on shutdown |


## Building

### Prerequisites

- Windows (x86_64): Thryve builds on Windows only, for now.
- Rust: the latest stable release. Nothing older than 1.98 can build it: the code uses `Path::is_empty`, new in 1.98.
- Cargo

### Build Commands

```bash
# Build in release mode
cargo build --release

# Run tests (some tests share process-wide state, so they run on one thread, as in CI)
cargo test --workspace -- --test-threads=1

# Build documentation
cargo doc --open
```

### Dependencies

Key dependencies:

- `crossbeam` - Lock-free concurrent data structures
- `crossbeam-skiplist` - Priority queue implementation, and the set of cancelled tasks
- `chrono` - Date/time formatting
- `serde` - Configuration deserialization
- `toml` - TOML parsing
- `thiserror` - Error types
- `petgraph` and `ron` - The task graph, and its export (graph mode)
- `windows-services` - Running as a Windows service

## Priority Queue Details

The priority queue uses an aging mechanism to prevent starvation:

```text
effective_priority = min(base_priority + aging_boost, max_priority)
```

Where `aging_boost` grows by one for every `aging_interval` seconds a task waits, so a low-priority task that has waited long enough comes before newer tasks with a higher priority. A task whose base priority is at least 80% of `max_priority` is *realtime*: realtime tasks always come before the others, and ageing never makes a task realtime. A follow-up that inherits its priority is capped just below realtime, so a chain never runs as realtime by inheritance.

## Log Output Format

```text
Log_Creation: 2024-01-15 10:30:45, [ LOGGING ], Event_Type: Log, Message: Hello
```

Each run gets a folder under `log/` in the root folder (the program's own folder unless `root_dir` names another), named after the time it started, with one file per log:
```text
log/
  2024_01_15_10_30/
    Main.log
    Error.log
    Debug.log       (debug mode only)
    <Name>.log      (one per name in `ctx.logging_file_names`)
```

If a shutdown cuts work short, one line says how much:

```text
thryve exited leaving 3 things not done: 1 lost tasks, 0 lost notifications, 1 lost task results, 1 still running
```

It counts the tasks that never ran, the results that carried something (a message, a chained task to start, or an error nobody took, including one handed to a follow-up, a join or a fan-out group's followers that never ran, counted once however many followers shared it), the notifications that were never delivered, and the threads still running when the grace period ran out. A task that was cancelled isn't lost work, so it isn't counted; a cancel that arrives once the shutdown has started is dropped, though, so its task still counts. The line goes to `Error.log`, or to `Main.log` when the Windows service was stopped, since stopping mid-work is expected there. With debugging on, `Debug.log` lists what was lost. A shutdown that lost nothing writes no line.

## License

Thryve is dual-licensed under two models:

### Open-Source License (AGPL-3.0)

For open-source projects and personal use, Thryve is available under the **GNU Affero General Public License v3.0**.

This means you can:
- ✅ Use Thryve freely in open-source projects
- ✅ Modify the source code
- ✅ Distribute the software
- ✅ Use it for learning and research

Conditions:
- 🔹 You must release your modifications under AGPL-3.0
- 🔹 You must make source code available when distributing
- 🔹 If used over a network (SaaS), you must make source code available to users

See the [`LICENSE`](./LICENSE) file for the full license text.

### Commercial License

For closed-source commercial projects that cannot comply with AGPL-3.0 requirements:

- 🔒 Keep your source code private
- 🔒 No requirement to disclose modifications
- 🔒 Suitable for proprietary SaaS applications

**Contact for licensing:** [thryve-framework.com](https://thryve-framework.com)

See [`COMMERCIAL.md`](COMMERCIAL.md) for more information.

---

**Choosing a license:**
- Open-source project? → Use **AGPL-3.0** (free)
- Closed-source commercial product? → Need **Commercial License** (paid)
- Unsure? → Contact us for guidance

## Contributing

Contributions are welcome! This project uses a **Contributor License Agreement (CLA)** to ensure that contributors grant necessary rights for both open-source and commercial licensing.

### Before You Contribute

1. **Read the CLA**: [`CLA-INDIVIDUAL.md`](CLA-INDIVIDUAL.md). Your first pull request asks you to sign it (see [below](#cla-contributor-license-agreement))
2. **Understand the architecture**: Review the [Architecture](#architecture) section
3. **Check existing issues**: See if your feature/bug is already discussed

### How to Contribute

#### 1. Fork and Clone

```bash
git clone https://github.com/thryveframework/thryve.git
cd thryve
```

#### 2. Create a Branch

```bash
git checkout -b feature/your-feature-name
# or
git checkout -b fix/your-bug-fix
```

#### 3. Make Your Changes

Follow Rust best practices:
- Write clear, idiomatic Rust code
- Add documentation comments (`///`) for public APIs
- Ensure code is formatted: `cargo fmt --all`
- Run clippy: `cargo clippy --workspace --all-targets -- -D warnings`

#### 4. Test Your Changes

```bash
# Run all tests, on one thread, as CI does
cargo test --workspace -- --test-threads=1
```

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for where tests go.

#### 5. Submit a Pull Request

- Provide a clear description of changes
- Reference related issues
- Ensure CI passes
- Sign the CLA when the bot asks (only on your first pull request)
- Address review feedback

### CLA (Contributor License Agreement)

The first time you open a pull request, a bot asks you to sign the [CLA](CLA-INDIVIDUAL.md) with a comment. Signing once covers all your contributions; [`CONTRIBUTING.md`](CONTRIBUTING.md#cla-contributor-license-agreement) has the steps, and what to do if your employer owns your work. By signing it, you:

1. **Grant License**: You grant Youssef Mostafa Abdellatif, who maintains Thryve, a perpetual, worldwide, non-exclusive, royalty-free license to use, modify, and distribute your contribution
2. **Ownership Representation**: Your contributions are your original work or you have permission to contribute them
3. **Patents**: You grant patent rights for your contributions

The CLA ensures that:
- Contributors retain copyright ownership
- Maintainers can include contributions in both AGPL and Commercial licenses
- The project can be commercially licensed without legal barriers

### Code Style

- Follow [Rust API Guidelines](https://rust-lang.github.io/api-guidelines/)
- Use `cargo fmt` for formatting
- Document public items with `///` comments
- Use meaningful variable and function names

### Questions?

- Open an issue for discussions
- Contact maintainers for architectural questions

---

**Thank you for contributing to Thryve!**
