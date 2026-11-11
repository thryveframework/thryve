# Thryve FFI — C-Compatible Foreign Function Interface

[![License](https://img.shields.io/badge/license-AGPL--3.0%20%2F%20Commercial-blue.svg)](../../LICENSE)

Thryve FFI is distributed under the same dual-license terms as the rest of the Thryve workspace: **AGPL-3.0-or-later** for open-source use, or a **Commercial License** for closed-source integration. See [`LICENSE`](../../LICENSE) and [`COMMERCIAL.md`](../../COMMERCIAL.md).

C-compatible wrapper functions for the Thryve framework, enabling integration with other programming languages, such as C, C++ and Python.

## Table of Contents

- [Overview](#overview)
- [Design Principles](#design-principles)
- [Quick Start](#quick-start)
- [Types](#types)
- [Callback Types](#callback-types)
- [Typed Tasks](#typed-tasks)
- [Observers](#observers)
- [Errors](#errors)
- [Memory Management](#memory-management)
- [Build System](#build-system)
- [API Reference](#api-reference)
- [Complete Examples](#complete-examples)

---

## Overview

The FFI layer exposes the Thryve framework's core types and functions through a C-compatible interface. This allows applications written in any language that supports C FFI to use Thryve without modifying Rust code.

The FFI layer handles:
- **Opaque pointers**: All complex Rust types are behind opaque pointer types, such as `ThryveCtx` and `ThryveAdvHandle`
- **Memory safety**: Explicit ownership transfer rules prevent double-frees and leaks
- **Error handling**: Status codes and null pointers for error detection, and `thryve_last_error()` for the reason
- **Callback management**: Function pointers for plugin, observer, and task logic
- **Typed tasks**: Tasks whose job returns a value or an error, which the engine hands to whoever follows them: follow-ups, joins, splits, cancelling and fan-out

---

## Design Principles

1. **Opaque pointers**: Each Rust type is behind a pointer to a struct that's only declared, never defined, so C code can't look inside it, and passing a `ThryveTask` where a `ThryveCtx` is expected doesn't compile.

2. **Explicit memory management**: Every `thryve_*_create` function has a corresponding `thryve_*_free` function. The foreign language is responsible for calling the free function when done, unless a call took the object over (see [Memory Management](#memory-management)).

3. **C-compatible calling convention**: All functions use `extern "C"` to ensure cross-language compatibility.

4. **Error handling**: Functions return `ThryveStatus` codes or `NULL` pointers on failure, and `thryve_last_error()` says why the latest call on the thread failed. See [Errors](#errors).

5. **Ownership transfer**: When passing pointers to Rust functions, ownership may be transferred. See [Memory Management](#memory-management) for details.

---

## Quick Start

The library comes prebuilt for Windows on Thryve's [release page](https://github.com/thryveframework/thryve/releases/latest): `thryve_ffi-<version>-windows-x86_64` has the headers and the library, and `thryve-<version>-windows-x86_64` adds `Topology.exe` next to the DLL. Each comes as .zip and .tar.gz. To build it yourself, see [Building](#building). What changed in each release is in the [changelog](https://github.com/thryveframework/thryve/blob/main/CHANGELOG.md).

Here's a minimal C example showing how to use the FFI layer. Note that a `PacketHandle` (obtained from `ctx`) is required for plugin creation:

```c
#include "thryve.h"

// Plugin cleanup function — called once at shutdown. A plugin with nothing to clean up
// can pass NULL instead.
int my_plugin_cleanup(const void* core_ptr) {
    // Cleanup logic here
    return ThryveStatus_Ok;  // any other value is logged to Error.log as a failed cleanup
}

// Plugin entry point — runs once when the engine starts. It fills in the result it's
// given, which starts empty: left alone, it reports nothing.
void my_plugin_entry_point(ThryveTaskResult* result, void* core_ptr) {
    result->result_type = "Started";
    result->message = "Plugin started";
    result->log_file = "Main";
}

// Shutdown task job — a "Fin" result_type is what tells the engine to
// finalize and exit its loop. There is no separate "stop" FFI call.
void shutdown_task_job(ThryveTaskResult* result, void* user_data) {
    result->result_type = "Fin";
}

int main(void) {
    // 1. Create configuration
    ThryveConfig config = thryve_config_default();
    if (!config) return 1;

    // 2. Create context from configuration
    ThryveCtx ctx = thryve_ctx_create(config);
    if (!ctx) {
        thryve_config_free(config);
        return 1;
    }

    // 3. Get the engine's connected PacketHandle from ctx
    ThryvePacketHandle packet_handle = thryve_ctx_get_packet_handle(ctx);
    if (!packet_handle) {
        thryve_ctx_free(ctx);
        thryve_config_free(config);
        return 1;
    }

    // 4. Create a plugin (PacketHandle is required)
    ThryvePlugin plugin = thryve_plugin_create(
        "MyType",             // type
        NULL,                 // plg_core_ptr (opaque pointer)
        packet_handle,        // tx (packet handle - REQUIRED)
        my_plugin_cleanup,    // clean_up callback
        my_plugin_entry_point // entry_point callback
    );

    // 5. Add plugin to context (takes ownership of the plugin)
    thryve_ctx_add_plugin(ctx, plugin);

    // 6. Start the engine. This consumes ctx — never touch or free ctx again.
    ThryveRunHandle handle = thryve_run_interactive_start(ctx);
    if (handle) {
        // Submit a shutdown task so the engine finalizes and its loop
        // exits — without this, thryve_run_interactive_free would hang forever.
        ThryveTask shutdown_task = thryve_task_create("Shutdown", 0, shutdown_task_job, NULL, NULL);
        if (shutdown_task) {
            thryve_packet_handle_send_task(packet_handle, shutdown_task);
        }
        thryve_run_interactive_free(handle);
    }

    // 7. Cleanup - plugin is already in ctx, and ctx was already consumed by
    // thryve_run_interactive_start, so don't free either of them again
    thryve_packet_handle_free(packet_handle);
    thryve_config_free(config);

    return 0;
}
```

---

## Types

### Opaque Pointers

Each opaque pointer type points to a struct the header only declares, so foreign languages treat it as an opaque handle.

| Type | Rust Equivalent | Description |
|------|-----------------|-------------|
| `ThryveConfig` | `*mut Config` | Configuration instance |
| `ThryveCtx` | `*mut Ctx` | Context (engine) instance |
| `ThryveRunHandle` | `*mut JoinHandle<()>` | Thread handle for interactive mode |
| `ThryveService` | `*mut Thryve` | Windows service instance |
| `ThryvePlugin` | `*mut Plugin` | Plugin instance |
| `ThryveObserver` | `*mut Observer` | Observer instance |
| `ThryvePacketHandle` | `*mut PacketHandle` | Packet handle for sending tasks/notifications |
| `ThryveTask` | `*mut Task` | Task instance |
| `ThryveNotif` | `*mut Notif` | Notification instance |
| `ThryveAdvTask` | `*mut AdvTask` | Typed task that hasn't been sent yet |
| `ThryveAdvHandle` | `*mut AdvTaskHandle` | Sent typed task's handle: what follow-ups, joins, fan-out and cancels take |
| `ThryveFanoutGroup` | `*mut FanoutGroup` | Followers of one task's result, declared together |
| `ThryveOutcome` | — | What a typed task's job returns: a value, an error, or a task to forward to |
| `ThryveHandoff` | — | What a follow-up's job gets: the result of the task or tasks it follows |

`thryve_service_create` doesn't return a bare opaque pointer — it returns a small `#[repr(C)]` struct pairing a service with its matching context:

```c
typedef struct {
    ThryveService service;
    ThryveCtx ctx;
} ThryveServiceCreateResult;
```

### Status Codes

```c
typedef enum {
  ThryveStatus_Ok = 0,
  ThryveStatus_NullArg = 1,
  ThryveStatus_AllocFail = 2,
  ThryveStatus_InvalidState = 3,
  ThryveStatus_OperationFail = 4,
  ThryveStatus_StrConvError = 5,
} ThryveStatus;
```

### Error Codes

A typed task's error carries a code of the job's choosing. Thryve's own codes are negative:

```c
#define THRYVE_ERROR_NO_OUTCOME -1     // the job returned NULL instead of an outcome
#define THRYVE_ERROR_WRONG_OUTCOME -2  // a value from a forwarding task, or a task to forward to from one that doesn't forward
```

### Task Result

```c
typedef struct {
  const char *result_type;  // The result's type, such as "Saved"; "Fin" stops the engine
  const char *message;      // A message to log
  const char *log_file;     // The log file to write the message to, such as "Main"
  bool has_chain;           // true if chain_task should run next
  ThryveTask chain_task;    // The task to run next
} ThryveTaskResult;
```

#### Filling in a Task Result

A plain task's job and a plugin's entry point get a pointer to a result to fill in. It starts empty, every field NULL or false, which reports nothing: a job with nothing to say leaves it alone. A NULL string is an empty one.

```c
void save(ThryveTaskResult* result, void* report) {
    if (!save_report(report)) {
        result->result_type = "SaveFailed";
        result->message = "the report couldn't be saved";
        result->log_file = "Main";
    }
}
```

The helper functions build a whole result, for the job to copy in:

```c
*result = thryve_task_result_new("Saved", "Report saved", "Main");
*result = thryve_task_result_new_with_chain("Saved", "Report saved", "Main", upload_task);
```

To run another task next, set both chain fields:

```c
result->has_chain = true;
result->chain_task = thryve_task_create("Upload", 2, upload, NULL, NULL);
```

**Important**:
- Thryve copies the strings as soon as the job returns, so they must still be valid then: string literals work, a buffer local to the job doesn't.
- A message needs a result type: without one, `Error.log` gets an error instead. A message for a log file that wasn't registered goes to `Error.log`, with a note.
- The chained task is Thryve's once the job returns: don't free it.
- Jobs fill in a result instead of returning one so that languages whose callbacks can't return a struct, such as Python's ctypes, can write them.

---

## Callback Types

### Task Callbacks

```c
// A plain task's job: fills in the task's result, which starts empty, and gets the
// user data given to thryve_task_create
typedef void (*ThryveTaskJob)(ThryveTaskResult *result, void *user_data);

// A plugin's entry point: runs once when the engine starts. It fills in a result like
// a task's job, and gets the plugin's core pointer (plg_core_ptr)
typedef void (*ThryvePluginEntryPoint)(ThryveTaskResult *result, void *plg_core_ptr);

// Plugin cleanup — called once when the engine shuts down, after every task has stopped
// Takes a pointer to the plugin's core data (plg_core_ptr)
// Returns ThryveStatus_Ok (0) if the cleanup worked; any other value means it failed,
// and Thryve logs that the plugin failed to clean up to Error.log
typedef int (*ThryvePluginCleanUp)(const void* core_ptr);
```

A task's job, a plugin's entry point and an observer's job are required: a create given NULL for one returns NULL. A clean-up or a free function may be NULL, when there's nothing to clean up or free.

### Typed Task Callbacks

```c
// A typed task's job: gets its user data, returns the task's outcome
typedef ThryveOutcome (*ThryveAdvJob)(void* user_data);

// The job of a follow-up, a join or a fan-out follower: gets the handoff of the
// task or tasks it follows, and its user data
typedef ThryveOutcome (*ThryveThenJob)(ThryveHandoff handoff, void* user_data);

// The job of a piece from thryve_packet_handle_split_into: gets where its items
// start, how many it has, and the user data every piece shares
typedef ThryveOutcome (*ThryveSplitJob)(size_t start, size_t len, void* user_data);

// The job of a piece from thryve_packet_handle_send_split_tasks: gets its chunk,
// its index, and the user data every piece shares
typedef ThryveOutcome (*ThryveChunkJob)(void* chunk, size_t index, void* user_data);

// Frees user data, a value or a chunk once Thryve is done with it. NULL: never freed
typedef void (*ThryveFreeFn)(void* data);
```

### Observer Callbacks

```c
// Observer job — the observer's whole loop, called once on its own thread.
// It borrows the observer until it returns: see Observers below.
typedef void (*ThryveObsJob)(ThryveObserver observer);

// Observer cleanup — called once when the engine shuts down, on a thread of its own
// Takes a pointer to the observer's core data (obs_core_ptr). NULL: nothing to clean up
typedef void (*ThryveObserverCleanUp)(const void* core_ptr);
```

Notifications take no callback: a notification carries its sender's data as `notif_core_ptr`, which the observer reads with `thryve_notification_core`.

---

## Typed Tasks

A typed task's job returns a value or an error. The engine hands that result to whoever follows the task, so tasks never need to know about each other: Hash doesn't know LoadImg follows it, and LoadImg only knows it gets a digest.

### Outcomes

A job returns a `ThryveOutcome`, built with one of:

- `thryve_outcome_value(value, free_value)`: a value. Thryve never reads it; it carries the pointer to whoever follows the task, and frees it with `free_value` once nobody can take it any more. A NULL `free_value` means it's never freed, for a value that needs no freeing, such as a number carried in the pointer itself.
- `thryve_outcome_error(code, message)`: an error. The message is copied, so it can be a buffer the job reuses. If nobody follows the task, `Error.log` gets the task's name, the message and the code.
- `thryve_outcome_forward(handle)`: the handle of another typed task, from a [forwarding](#forwarding) task.

A job that returns NULL fails with the code `THRYVE_ERROR_NO_OUTCOME`. An outcome that a job builds but doesn't return is freed with `thryve_outcome_free`.

### A First Pipeline: Hash, then LoadImg

```c
// Hash: the file's digest, as the task's value. Thryve frees it with `free`.
ThryveOutcome hash_file(void* user_data) {
    uint64_t* digest = malloc(sizeof *digest);
    if (digest == NULL) {
        return thryve_outcome_error(1, "out of memory");
    }
    *digest = fnv1a((const char*)user_data);
    return thryve_outcome_value(digest, free);
}

// LoadImg: decodes the image whose digest it was handed. Hash's error is passed on.
ThryveOutcome load_img(ThryveHandoff digest, void* user_data) {
    if (thryve_handoff_is_error(digest)) {
        return thryve_outcome_error(thryve_handoff_error_code(digest),
                                    thryve_handoff_error_message(digest));
    }
    const uint64_t* hash = thryve_handoff_value(digest, 0);
    return thryve_outcome_value(decode(*hash), free_image);
}

// Hash's user data is its path, freed with `free` once Hash has run.
ThryveAdvTask hash = thryve_adv_task_create("Hash", 2, hash_file, strdup("photo.png"), free);
ThryveAdvHandle digest = thryve_packet_handle_send_adv_task(packet_handle, hash);
ThryveAdvHandle image = thryve_adv_handle_then(digest, "LoadImg", load_img, NULL, NULL);
```

`thryve_adv_handle_then` declares the follow-up at once. It waits in the engine until Hash finishes, so it can be declared before or after Hash runs. It uses up `digest` (a task has one follow-up) and returns LoadImg's own handle, so steps can be chained. The follow-up inherits the task's priority, capped below realtime; `thryve_adv_handle_then_with_priority` gives it a priority of its own.

Every handle has to end somewhere: in a call that uses it up, or in `thryve_adv_handle_free`, which says nobody will follow the task. The task still runs; then its value is freed, or its error written to `Error.log`.

### Reading a Handoff

A follow-up's job reads its input through the `ThryveHandoff` it's given, which is only valid while the job runs:

| Function | Returns |
|----------|---------|
| `thryve_handoff_is_error(handoff)` | Whether the task (or one of the joined tasks) failed |
| `thryve_handoff_value_count(handoff)` | 1 for a follow-up or a fan-out follower, one per task for a join, 0 for an error |
| `thryve_handoff_value(handoff, index)` | A value, read in place: index 0, or the task's place in a join |
| `thryve_handoff_take_value(handoff, index)` | A value the job takes over: Thryve won't free it, so the job frees it, or returns it as its own value |
| `thryve_handoff_error_code(handoff)` | The error's code |
| `thryve_handoff_error_message(handoff)` | The error's message, valid until the job returns |
| `thryve_handoff_error_position(handoff)` | For a join: which task failed, counted from 0 |

A value the job doesn't take is freed once the job returns. To hand a value on as the job's own, take it first:

```c
ThryveOutcome save(ThryveHandoff image, void* user_data) {
    void* pixels = thryve_handoff_take_value(image, 0);  // ours now
    write_to_disk(pixels);
    return thryve_outcome_value(pixels, free_image);    // handed on to whoever follows Save
}
```

### Forwarding

A forwarding task's job creates and sends another typed task, and returns that task's handle with `thryve_outcome_forward`. Whoever follows the forwarding task then gets that task's value or error. It's for a last step that only exists once the task runs, such as one picked by what the task found:

```c
ThryveOutcome prepare(void* user_data) {
    const Photo* photo = user_data;
    ThryveAdvTask next = photo->size > 1000000
        ? thryve_adv_task_create("Shrink", 2, shrink, (void*)photo, NULL)
        : thryve_adv_task_create("Keep", 2, keep, (void*)photo, NULL);
    return thryve_outcome_forward(thryve_packet_handle_send_adv_task(g_packet_handle, next));
}

ThryveAdvTask task = thryve_adv_task_create_forward("Prepare", 2, prepare, photo, free_photo);
```

`thryve_adv_handle_then_forward` declares a follow-up that forwards the same way. A forwarding job can also fail with `thryve_outcome_error`, and nothing is forwarded. A forwarding job that returns a value, or a task to forward to from a task that doesn't forward, fails with `THRYVE_ERROR_WRONG_OUTCOME`.

### Joins

`thryve_adv_handles_join` gives several tasks one follow-up. It runs once all of them have finished, with all their values in the array's order, or as soon as one fails, with that task's error and position:

```c
ThryveOutcome report(ThryveHandoff parts, void* user_data) {
    if (thryve_handoff_is_error(parts)) {
        fprintf(stderr, "part %zu failed: %s\n", thryve_handoff_error_position(parts),
                thryve_handoff_error_message(parts));
        return thryve_outcome_error(thryve_handoff_error_code(parts),
                                    thryve_handoff_error_message(parts));
    }
    const Archive* archive = thryve_handoff_value(parts, 0);
    const Thumb* thumbnail = thryve_handoff_value(parts, 1);
    /* ... */
    return thryve_outcome_value(NULL, NULL);
}

ThryveAdvHandle parts[2] = {archive, thumbnail};
ThryveAdvHandle done = thryve_adv_handles_join(parts, 2, "Report", report, NULL, NULL);
// parts[0] and parts[1] are NULL now: the join used them up.
```

The join inherits its tasks' highest priority, capped below realtime; `thryve_adv_handles_join_with_priority` gives it its own. A NULL entry, or a handle given twice, makes the join fail.

### Splits

`thryve_packet_handle_split_into` cuts `item_count` items into at most `number_of_pieces` pieces, and sends one task per piece. The cut is balanced (sizes differ by at most one item, and no piece is empty), and the items stay where they are: each piece gets where its items start and how many it has, and reads them through the user data every piece shares.

```c
ThryveOutcome sum_rows(size_t start, size_t len, void* user_data) {
    const Table* table = user_data;
    uint64_t* sum = malloc(sizeof *sum);
    *sum = 0;
    for (size_t row = start; row < start + len; row++) {
        *sum += table->rows[row];
    }
    return thryve_outcome_value(sum, free);
}

ThryveAdvHandle pieces[4];
size_t piece_count = 0;
if (thryve_packet_handle_split_into(packet_handle, table->row_count, "SumRows", 4, 2, sum_rows,
                                    table, free_table, pieces, &piece_count) == ThryveStatus_Ok) {
    ThryveAdvHandle total = thryve_adv_handles_join(pieces, piece_count, "TotalRows", add_sums, NULL, NULL);
}
```

For a cut of your own (by size, at content boundaries, over files), `thryve_packet_handle_send_split_tasks` sends one task per chunk you give it. Each piece gets its own chunk, which is freed with `free_chunk` once its job has run.

### Fan-out

A task's result normally goes to one follow-up. A fan-out group lets several followers share it: say LoadImg decodes photo.png, and Thumbnail and Metadata both need its pixels.

```c
ThryveFanoutGroup group = thryve_adv_handle_fan_out(image);
ThryveAdvHandle thumbnail = thryve_fanout_then(group, "Thumbnail", make_thumbnail, NULL, NULL);
ThryveAdvHandle metadata = thryve_fanout_then(group, "Metadata", read_metadata, NULL, NULL);
thryve_fanout_send(group);
```

The followers read the one value in place, at the same time, so they get it as `const`, must not change it, and can't take it. It's freed once the last follower is done. Each follower's handle is an ordinary handle: it can be followed, joined or cancelled. `thryve_fanout_send` uses the group up; a group with no follower sends nothing, and its task's result is let go.

### Cancelling

`thryve_adv_handle_cancel` cancels a task and uses its handle up. A task that hasn't started never runs, and its user data is freed. A running task can't be stopped: it finishes, and its result is let go. The work only that task needed stops too: cancelling LoadImg also cancels Hash, if Hash hasn't started, since nobody else can take Hash's result. A fan-out follower only withdraws from its group.

`thryve_adv_handles_cancel_all` cancels every task in an array, such as a split's pieces, and sets each entry to NULL.

### Who Owns What

- **User data and chunks** are Thryve's once passed. Each is freed once, with the free function given with it: after its job has run, or when its task is dropped without running, even when the call that took it fails.
- **Values** are Thryve's until a job takes one. Thryve frees each once, with its free function.
- **Handles** are used up by every call that takes one, even one that fails. Joins and cancels set the array entries they took to NULL.
- **A handoff** is only valid during its job; so is the error message read from it.
- **Names** are copied: the caller keeps its strings.

### Threads

- Jobs run on Thryve's plugin workers, several at a time.
- Free functions run wherever Thryve lets the memory go: on a worker, or on the engine's own thread. They must only free memory: a free function that sends a task could make the engine wait for itself.

---

## Observers

An observer's job is its whole loop. The engine starts the observer's thread when it loads the observer, and the thread calls the job once. The job loops on `thryve_observer_pop`, which waits for the next notification sent to the observer, and returns NULL once the engine shuts the observer down; the job then returns, which ends the thread.

```c
void my_observer_job(ThryveObserver observer) {
    Stats* stats = thryve_observer_core(observer);  // the obs_core_ptr it was created with

    ThryveNotif notif;
    while ((notif = thryve_observer_pop(observer)) != NULL) {
        Reading* reading = thryve_notification_core(notif);  // the notif_core_ptr it was sent with
        record(stats, reading);
        free(reading);
        thryve_notification_free(notif);
    }
}
```

- The `ThryveObserver` the job gets belongs to the engine. It stays valid until the job returns, and the job must not keep it or free it. Older versions of these docs asked the job to free it with `thryve_observer_free`; on the observer a job was given, that now does nothing.
- Each popped notification is the job's to free with `thryve_notification_free`. Thryve never frees the data a notification carries.
- The shutdown calls the observer's `clean_up` before `thryve_observer_pop` returns NULL. So if `clean_up` frees the observer's core data, the job must not use it once `thryve_observer_pop` has returned NULL.

---

## Errors

A function that fails returns NULL or a `ThryveStatus` other than `ThryveStatus_Ok`. `thryve_last_error()` then says why: the latest call's error on the calling thread, as a null-terminated string, or NULL if that call didn't fail.

```c
ThryveTask task = thryve_task_create("", 2, my_job, NULL, NULL);
if (task == NULL) {
    fprintf(stderr, "no task: %s\n", thryve_last_error());
    // no task: task name must not be empty
}
```

- Every Thryve call clears it first, so read it right after the call that failed.
- The string stays valid until the next Thryve call on the same thread.
- For an error Thryve returned, it's the error's text with its causes, as the logs show it. For a NULL argument, it names the argument: `handle is NULL`, or `job is NULL` for a missing job.
- Names and types must be valid UTF-8: a create given one that isn't returns NULL, and says which argument (`name isn't valid UTF-8`).

A typed task that fails returns an error with its own code and message instead: see [Outcomes](#outcomes).

---

## Memory Management

### Ownership Transfer Rules

When passing data to Rust functions, ownership may be transferred. Follow these rules:

1. **Callback pointers**: `entry_point`, `job` and the other callbacks are wrapped in Rust closures and called from Thryve's threads. They must stay callable for as long as the engine runs.

2. **String pointers**: `name`, `type`, and similar string arguments are copied. The caller keeps its strings, and can free them once the call returns.

3. **Plugin/Observer creation**: After calling `thryve_plugin_create` or `thryve_observer_create`, the plugin/observer is yours until it's added to a context. Free one that's never added with `thryve_plugin_free` or `thryve_observer_free`.

4. **Context addition**: After calling `thryve_ctx_add_plugin` or `thryve_ctx_add_observer`, the plugin/observer is moved into the context. **Do not free it** — the context will clean it up.

5. **PacketHandle**: Get it from `thryve_ctx_get_packet_handle(ctx)` — the engine's real, connected handle. It is **cloned** inside the Rust side, so the foreign language can continue to use its copy after passing to plugin/observer creation.

6. **Sending tasks/notifications**: `thryve_packet_handle_send_task`, `thryve_packet_handle_send_notification` and `thryve_packet_handle_send_adv_task` all **take ownership** of the task/notification passed in. **Do not** free it afterward. This holds even when the send fails: with a NULL handle, before the engine is started, or once it's gone, they fail and drop what was passed in.

7. **Start, then send**: a context's packet handle refuses tasks and notifications (`InvalidState`) until `thryve_run_interactive_start` or `thryve_service_run` has started the engine. Take the handle with `thryve_ctx_get_packet_handle` before the start (the start uses up `ctx`), and send after it, as the example above does. Sending from inside a job or a plugin's entry point always works: those run once the engine has started.

8. **Core pointers stay yours**: the `*_core_ptr` given to a plugin, an observer or a notification is never freed by Thryve. It's handed back to your clean-up callback, and to `thryve_observer_core` or `thryve_notification_core`.

9. **User data, values and chunks**: given to Thryve with a plain task or a typed one, they're freed by Thryve, once, with the free function given with them; typed tasks' handles are used up by every call that takes them. See [Who Owns What](#who-owns-what).

10. **`thryve_ctx_create` vs `thryve_service_create`**: `thryve_ctx_create(config)` is for **interactive mode** only. Service mode doesn't use it — `thryve_service_create(config)` already returns its own matching `ctx` bundled together with the service (see [`ThryveServiceCreateResult`](#types)).

11. **`thryve_run_interactive_start`**: consumes `ctx` unconditionally. After calling it, `ctx` must never be freed or otherwise used again.

### Ownership Transfer Diagram

```
Foreign Language                              Rust
────────────────                              ────
                                              ┌───────────────────────────┐
1. thryve_config_default()                    │ Creates and owns Config   │
   → ThryveConfig                             └───────────────────────────┘
                                              ┌───────────────────────────┐
2. thryve_ctx_create(config)                  │ Creates and owns Ctx      │
   → ThryveCtx                                └───────────────────────────┘
                                              ┌───────────────────────────┐
3. thryve_plugin_create(type, core, tx,       │ Copies type               │
   clean_up, entry_point)                     │ Wraps entry_point and     │
   → ThryvePlugin                             │ clean_up                  │
                                              │ Keeps plg_core_ptr for    │
                                              │ clean_up; never frees it  │
                                              └───────────────────────────┘
                                              ┌───────────────────────────┐
4. thryve_ctx_add_plugin(ctx, plugin)         │ Moves the plugin into ctx │
   → ThryveStatus                             │ Don't free it afterwards  │
                                              └───────────────────────────┘
                                              ┌───────────────────────────┐
5. thryve_plugin_free(plugin)                 │ Frees a plugin that was   │
   (only if never added to a ctx)             │ never added to a ctx      │
                                              └───────────────────────────┘
```

### Free Functions

Every `create` function has a corresponding `free` function:

| Create Function | Free Function |
|-----------------|---------------|
| `thryve_config_default()` / `thryve_config_from_file()` | `thryve_config_free()` |
| `thryve_ctx_create()` (interactive mode only) | `thryve_ctx_free()` (unless consumed by `thryve_run_interactive_start`) |
| `thryve_plugin_create()` | `thryve_plugin_free()` (unless added to a context) |
| `thryve_observer_create()` | `thryve_observer_free()` (unless added to a context) |
| `thryve_task_create()` | `thryve_task_free()` (unless already sent via a `*_send_task` call) |
| `thryve_service_create()` → `.service` | `thryve_service_free()` (unless consumed by `thryve_service_run`) |
| `thryve_service_create()` → `.ctx` | `thryve_ctx_free()` (unless consumed by `thryve_service_run`) |
| `thryve_ctx_get_packet_handle()` / `thryve_packet_handle_clone()` | `thryve_packet_handle_free()` |
| `thryve_notification_create()` / `thryve_observer_pop()` | `thryve_notification_free()` (unless already sent via a `*_send_notification` call) |
| `thryve_adv_task_create()` / `thryve_adv_task_create_forward()` | `thryve_adv_task_free()` (unless already sent) |
| A `ThryveAdvHandle` | `thryve_adv_handle_free()` (unless a call used it up) |
| `thryve_outcome_*()` | `thryve_outcome_free()` (unless a job returned it) |
| `thryve_adv_handle_fan_out()` | `thryve_fanout_send()`, which always uses the group up |

There is no `thryve_packet_handle_create()` — a standalone packet handle would not be wired to any engine. `thryve_ctx_get_packet_handle(ctx)` is the only way to obtain one.

---

## Build System

The FFI library uses [cbindgen](https://github.com/mozilla/cbindgen) to automatically generate C/C++ header files during the build process. This ensures the headers are always in sync with the Rust definitions.

### Auto-Generated Headers

When you build the FFI library with Cargo, the `build.rs` script automatically generates two header files in the `OUT_DIR`, and copies them to `out/`:

- **`thryve.h`** — The C-compatible header, generated by cbindgen. Include this from C code.
- **`thryve_ffi.h`** — The C++ wrapper header, generated by `build.rs`. It includes `thryve.h` inside `extern "C"` guards. Include this from C++ code.

cbindgen only declares a function marked `#[unsafe(no_mangle)]` as written, and the FFI functions are marked `#[cfg_attr(not(test), unsafe(no_mangle))]` so that test and coverage builds see plain Rust functions. So `build.rs` hands cbindgen a copy of the sources with the plain mark.

### Including in C/C++ Code

**C code:**
```c
#include "thryve.h"
```

**C++ code:**
```cpp
#include "thryve_ffi.h"  // Wrapper with extern "C"
```

### Building

Simply run:
```bash
cargo build --release
```

The headers are generated in the build script's `OUT_DIR` and copied to `crates/thryve_ffi/out/`: add `out/` to your include path. The library is in the workspace's `target/debug/` or `target/release/`:

| Platform | Shared library | Link against |
|----------|----------------|--------------|
| Windows, MSVC toolchain | `thryve_ffi.dll` | `thryve_ffi.dll.lib`, the DLL's import library |
| Windows, GNU toolchain | `thryve_ffi.dll` | `libthryve_ffi.dll.a`, or the DLL itself with gcc |

Thryve builds on Windows only, for now.

`thryve_ffi.lib` and `libthryve_ffi.a` are static libraries: they build the whole library into your program, and need Rust's system libraries to link, so link the shared library instead. The release downloads hold `include/`, `bin/` (with `thryve_ffi.dll`) and `lib/`, where the import library is renamed `thryve_ffi.lib`.

### The Committed Header

`thryve.h` at the crate's root is a copy of the generated `out/thryve.h`, kept in the repository so the C API can be read without building. After changing the FFI, build it and copy `out/thryve.h` over it; a test in the `thryve_tests` crate fails while they differ.

### Build System Files

The repository's [`examples/c_cpp`](../../examples/c_cpp) folder holds the build files for the C and C++ examples, which also show how to include the generated headers and link against the shared library. Both build against this repository's build of the library, or against a release download:

- **`CMakeLists.txt`** — finds the library in the workspace's `target/` folder, or in a release download named with `-DTHRYVE_FFI_DIR`; links the DLL's import library (MSVC or GNU), and copies the DLL next to each example. From `examples/c_cpp`, after `cargo build -p thryve_ffi`:
  ```bash
  cmake -S . -B build
  cmake --build build
  ```
- **`Makefile`** — GNU Make with gcc/g++ and a POSIX shell (MSYS2 on Windows): `make`, `make c`, `make cpp`, `make BUILD_TYPE=release`, or `make THRYVE_FFI_DIR=<folder>` for a release download. It builds the library first, and on Windows links the DLL itself and copies it next to each example.

---

## API Reference

### Errors

| Function | Description |
|----------|-------------|
| `thryve_last_error()` | Why the latest Thryve call on this thread failed, as a null-terminated C string, or NULL if it didn't fail. Valid until the next Thryve call on the same thread. |

### Config Wrappers

| Function | Description |
|----------|-------------|
| `thryve_config_default()` | Creates a default configuration instance. Returns `ThryveConfig` or NULL. |
| `thryve_config_from_file(path)` | Creates a configuration from a TOML file. `path` is a null-terminated C string. Returns `ThryveConfig`, or NULL if the file can't be read, doesn't parse (every key is required, unknown keys are rejected) or has invalid values. There is no fallback to the defaults; call `thryve_config_default()` if you want them. |
| `thryve_config_free(config)` | Frees a configuration instance. Safe to call with NULL. |

### PacketHandle Wrappers

| Function | Description |
|----------|-------------|
| `thryve_packet_handle_free(handle)` | Frees a PacketHandle instance. Safe to call with NULL. |
| `thryve_packet_handle_clone(handle)` | Clones a PacketHandle. Returns a new `ThryvePacketHandle`. |
| `thryve_packet_handle_send_task(handle, task)` | Sends a task through the handle. **Takes ownership of `task`.** Returns `ThryveStatus`: `InvalidState` if the engine isn't running: not started yet, or gone (the task is dropped). |
| `thryve_packet_handle_send_notification(handle, notif)` | Sends a notification through the handle. **Takes ownership of `notif`.** Returns `ThryveStatus`: `InvalidState` if the engine isn't running: not started yet, or gone (the notification is dropped). |

### Ctx Wrappers

| Function | Description |
|----------|-------------|
| `thryve_ctx_create(config)` | Creates a context from a configuration (interactive mode only — service mode gets its ctx from `thryve_service_create`). Returns `ThryveCtx` or NULL. |
| `thryve_ctx_free(ctx)` | Frees a context instance. Safe to call with NULL. Do not call on a `ctx` already consumed by `thryve_run_interactive_start`/`thryve_service_run`. |
| `thryve_ctx_get_packet_handle(ctx)` | Returns the engine's real, connected `ThryvePacketHandle` for this ctx — the only correct way to obtain one. It refuses sends until the engine is started, so take it before the start and send after. |
| `thryve_ctx_add_plugin(ctx, plugin)` | Moves a plugin into the context. Returns `ThryveStatus`. |
| `thryve_ctx_add_observer(ctx, observer)` | Moves an observer into the context. Returns `ThryveStatus`. |
| `thryve_ctx_set_logging_file_names(ctx, file_names, count)` | Sets logging file names. `file_names` is an array of null-terminated C strings. |

### Task Wrappers

| Function | Description |
|----------|-------------|
| `thryve_task_create(name, priority, job, user_data, free_user_data)` | Creates a plain task. `name` is a null-terminated C string, copied. `job` fills in the task's result and gets `user_data`, which `free_user_data` frees once (NULL: never). Returns `ThryveTask`, or NULL; `user_data` is freed either way. |
| `thryve_task_free(task)` | Frees a task that was never sent, and its user data. Safe to call with NULL. |
| `thryve_task_result_new(result_type, message, log_file)` | Builds a `ThryveTaskResult` with the specified fields, for a job to copy into its result. All parameters are null-terminated C strings. |
| `thryve_task_result_new_with_chain(result_type, message, log_file, chain_task)` | Builds a `ThryveTaskResult` with a chained task, for a job to copy into its result. |

### Typed Task Wrappers

| Function | Description |
|----------|-------------|
| `thryve_adv_task_create(name, priority, job, user_data, free_user_data)` | Creates a typed task whose `job` returns a value or an error. Returns `ThryveAdvTask`, or NULL; `user_data` is freed either way. |
| `thryve_adv_task_create_forward(name, priority, job, user_data, free_user_data)` | Creates a typed task whose `job` forwards to another typed task. Returns `ThryveAdvTask` or NULL. |
| `thryve_adv_task_free(task)` | Frees a typed task that was never sent, and its user data. Safe to call with NULL. |
| `thryve_packet_handle_send_adv_task(handle, task)` | Sends a typed task. **Takes ownership of `task`.** Returns its `ThryveAdvHandle`, or NULL if the engine isn't running (the task is dropped). |
| `thryve_adv_handle_then(handle, name, job, user_data, free_user_data)` | Declares the task's follow-up, which inherits its priority, capped below realtime. **Uses up `handle`.** Returns the follow-up's handle or NULL. |
| `thryve_adv_handle_then_with_priority(handle, name, priority, job, user_data, free_user_data)` | Like `then`, with a priority of its own. |
| `thryve_adv_handle_then_forward(handle, name, job, user_data, free_user_data)` | Like `then`, but the follow-up forwards. |
| `thryve_adv_handle_cancel(handle)` | Cancels the task, and the work only it needed. **Uses up `handle`.** Safe to call with NULL. |
| `thryve_adv_handle_free(handle)` | Lets a handle go without a follow-up: the task still runs, and its result is let go. Safe to call with NULL. |

### Join and Split Wrappers

| Function | Description |
|----------|-------------|
| `thryve_adv_handles_join(handles, count, name, job, user_data, free_user_data)` | Joins the tasks into one follow-up, which gets all their values or the first error. **Uses up every handle** and sets each entry to NULL. Returns the join's handle or NULL. |
| `thryve_adv_handles_join_with_priority(handles, count, name, priority, job, user_data, free_user_data)` | Like `join`, with a priority of its own. |
| `thryve_adv_handles_cancel_all(handles, count)` | Cancels every task in the array, and sets each entry to NULL. NULL entries are skipped. |
| `thryve_packet_handle_split_into(handle, item_count, name, number_of_pieces, priority, job, user_data, free_user_data, out_handles, out_count)` | Cuts `item_count` items into balanced pieces and sends one task per piece. `out_handles` needs room for `number_of_pieces` handles. Returns `ThryveStatus`. |
| `thryve_packet_handle_send_split_tasks(handle, chunks, chunk_count, free_chunk, name, priority, job, user_data, free_user_data, out_handles)` | Sends one task per chunk. `out_handles` needs room for `chunk_count` handles. Returns `ThryveStatus`. |

### Fan-out Wrappers

| Function | Description |
|----------|-------------|
| `thryve_adv_handle_fan_out(handle)` | Starts a fan-out group on the task's result. **Uses up `handle`.** Returns `ThryveFanoutGroup` or NULL. |
| `thryve_fanout_then(group, name, job, user_data, free_user_data)` | Declares a follower, which reads the shared value in place. Returns the follower's handle or NULL. |
| `thryve_fanout_then_with_priority(group, name, priority, job, user_data, free_user_data)` | Like `thryve_fanout_then`, with a priority of its own. |
| `thryve_fanout_send(group)` | Sends the followers. **Uses up `group`.** Returns `ThryveStatus`: `InvalidState` if the group has no follower or the engine isn't running. |

### Outcome and Handoff Wrappers

| Function | Description |
|----------|-------------|
| `thryve_outcome_value(value, free_value)` | A job's value, freed with `free_value` once nobody can take it. |
| `thryve_outcome_error(code, message)` | A job's error. `message` is copied. |
| `thryve_outcome_forward(handle)` | A forwarding job's task to forward to. **Uses up `handle`.** |
| `thryve_outcome_free(outcome)` | Frees an outcome no job returned. Safe to call with NULL. |
| `thryve_handoff_is_error(handoff)` | Whether the handoff holds an error. |
| `thryve_handoff_value_count(handoff)` | How many values it holds. |
| `thryve_handoff_value(handoff, index)` | A value, read in place. |
| `thryve_handoff_take_value(handoff, index)` | A value the job takes over. NULL for a fan-out follower, whose value is shared. |
| `thryve_handoff_error_code(handoff)` | The error's code, or 0. |
| `thryve_handoff_error_message(handoff)` | The error's message, or NULL. |
| `thryve_handoff_error_position(handoff)` | For a join that failed: which task failed. |

### Run Interactive Wrappers

| Function | Description |
|----------|-------------|
| `thryve_run_interactive_start(ctx)` | Starts the Thryve engine in interactive mode. Returns `ThryveRunHandle`, or NULL if the engine couldn't start (it stops every thread it started, and its error is in `Error.log` once the log folder exists). |
| `thryve_run_interactive_free(handle)` | Waits for the engine to finish and frees the handle. Returns `ThryveStatus`. |

### Service Wrappers

| Function | Description |
|----------|-------------|
| `thryve_service_create(config)` | Creates a service instance bundled with its matching ctx (for Windows service mode). Returns `ThryveServiceCreateResult { service, ctx }` — both NULL on failure. |
| `thryve_service_run(service, ctx)` | Runs the service (blocking until SCM sends Stop). **Takes ownership of both `service` and `ctx`.** Returns `ThryveStatus`: `Ok` once the service has stopped, `InvalidState` if the process wasn't started by the SCM, `OperationFail` if the engine couldn't start. |
| `thryve_service_free(service)` | Frees a service instance. Safe to call with NULL. Do not call on a `service` already consumed by `thryve_service_run`. |

### Plugin Wrappers

| Function | Description |
|----------|-------------|
| `thryve_plugin_create(type, core_ptr, tx, clean_up, entry_point)` | Creates a new plugin. `entry_point` runs once when the engine starts; `clean_up` may be NULL. Returns `ThryvePlugin` or NULL. |
| `thryve_plugin_free(plugin)` | Frees a plugin that was never added to a context. Safe to call with NULL. |

### Observer Wrappers

| Function | Description |
|----------|-------------|
| `thryve_observer_create(type, core_ptr, tx, clean_up, obs_job_fn)` | Creates a new observer. `clean_up` may be NULL. Returns `ThryveObserver` or NULL. |
| `thryve_observer_free(observer)` | Frees an observer that was never added to a context. Safe to call with NULL. Does nothing on the observer a job was given. |
| `thryve_observer_pop(observer)` | For the observer's job: waits for the next notification, and returns it. NULL once the observer is shut down. |
| `thryve_observer_core(observer)` | The `obs_core_ptr` the observer was created with. |

### Notification Wrappers

| Function | Description |
|----------|-------------|
| `thryve_notification_create(notif_type, receiver, core_ptr)` | Creates a new notification. Returns `ThryveNotif` or NULL. |
| `thryve_notification_free(notif)` | Frees a notification instance. Safe to call with NULL. |
| `thryve_notification_core(notif)` | The `core_ptr` the notification was created with, or NULL for one that wasn't created from C. Thryve never frees it. |

---

## FFI Adapter Implementations

The FFI layer uses adapter structs to bridge C function pointers with Rust traits:

### PluginLogicAdapter

Wraps `plg_type: String`, `core_ptr: *mut c_void` and `clean_up: ThryvePluginCleanUp`. Implements the `PlgLogic` trait: a callback that returns anything other than `ThryveStatus_Ok` becomes a `FinalizationError::PluginCleanup` naming the plugin's type. Marked `Send + Sync` for use in `Arc`.

### ObserverLogicAdapter

Wraps `core_ptr: *mut c_void` and `clean_up: ThryveObserverCleanUp`. Implements the `ObsLogic` trait. Marked `Send + Sync` for use in `Arc`.

### NotificationLogicAdapter

Wraps `core_ptr: *mut c_void`. Implements the `NotifLogic` trait. Marked `Send + Sync` for use in `Arc`.

---

## Complete Examples

The repository's [`examples/`](../../examples) folder holds two programs in each language, built from plugins, an observer and pipelines of typed tasks: a report on a web server's access log, and two projects in one engine (the same report, next to a count of primes). In the report, the plugin's entry point reads the log and splits its lines over 8 Parse tasks; Merge joins their counts; four sections share Merge's result in a fan-out; and Report joins the sections and sends the text to the observer, whose job loops on `thryve_observer_pop`, prints the reports, and ends the run.

- [`examples/c_cpp/c/`](../../examples/c_cpp/c), in C;
- [`examples/c_cpp/cpp/`](../../examples/c_cpp/cpp), in C++;
- [`examples/python/`](../../examples/python), in Python, through [`thryve.py`](../../examples/python/thryve.py): this API for Python, through `ctypes`. The [Python guide](docs/python-dev-guide.md) shows how to use it.

Build the C and C++ programs with the files in [Build System Files](#build-system-files). [`examples/README.md`](../../examples/README.md) says how to run each.

---

## Related Files

- [`src/lib.rs`](src/lib.rs) — Main FFI implementation
- [`src/typed_tasks.rs`](src/typed_tasks.rs) — Typed tasks, follow-ups, joins, splits, cancelling and fan-out
- [`Cargo.toml`](Cargo.toml) — Dependencies
- [`docs/cpp-dev-guide.md`](docs/cpp-dev-guide.md) — C++ developer guide
- [`docs/python-dev-guide.md`](docs/python-dev-guide.md) — Python developer guide, through `thryve.py`
- [`thryve_utils/src/traits/plugin_logic.rs`](../thryve_utils/src/traits/plugin_logic.rs) — PlgLogic trait
- [`thryve_utils/src/traits/observer_logic.rs`](../thryve_utils/src/traits/observer_logic.rs) — ObsLogic trait
- [`thryve_utils/src/traits/notif_logic.rs`](../thryve_utils/src/traits/notif_logic.rs) — NotifLogic trait
