# Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
#
# This file is part of Thryve, dual-licensed under the GNU Affero General
# Public License v3.0 (or later) and a separate Commercial License.
# See LICENSE and COMMERCIAL.md in the repository root, or visit
# https://thryve-framework.com for licensing options.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Thryve's C API for Python, through ctypes: nothing to install but Thryve's C library.

`load()` opens the library. `lib` then has every function in thryve.h, under its C name and with
its C types: handles are integers (None for NULL), strings are bytes, and statuses are the
`ThryveStatus` values below.

The rest of this module carries Python objects through Thryve, so that plugins, observers and
typed tasks can be written in Python:

- A task's value, a job, or a notification's data is a Python object. Thryve carries an id in its
  place, and this module holds the object until Thryve frees the id.
- A job is a Python function. A typed task's job returns its value, or raises its error:
  `TaskError` sets the error's code, any other exception has code 1. A plain task's job returns
  its result as (result_type, message, log_file), or None for none. Nothing escapes into Thryve.
- The calls that can fail raise `Error`, with Thryve's reason.

Thryve calls the jobs on its own threads.
"""

import ctypes
import itertools
import json
import os
import re
import sys
import threading
import traceback
from ctypes import (
    CFUNCTYPE,
    POINTER,
    Structure,
    byref,
    c_bool,
    c_char_p,
    c_int,
    c_int32,
    c_size_t,
    c_uint8,
    c_void_p,
)
from pathlib import Path

# ThryveStatus: what the calls that don't return a handle return.
OK = 0
NULL_ARG = 1
ALLOC_FAIL = 2
INVALID_STATE = 3
OPERATION_FAIL = 4
STR_CONV_ERROR = 5

# A job that returned no outcome, and one that returned the wrong kind.
ERROR_NO_OUTCOME = -1
ERROR_WRONG_OUTCOME = -2


class Error(Exception):
    """A call to Thryve failed; the message says why."""


class TaskError(Exception):
    """A typed task's error: its code, its message, and, in a join, the failed task's place."""

    def __init__(self, code, message, position=0):
        super().__init__(message)
        self.code = code
        self.message = message
        self.position = position


# ============================================================================
# The C API
# ============================================================================


class TaskResult(Structure):
    """ThryveTaskResult: what a plain task's job reports."""

    _fields_ = [
        ("result_type", c_char_p),
        ("message", c_char_p),
        ("log_file", c_char_p),
        ("has_chain", c_bool),
        ("chain_task", c_void_p),
    ]


class ServiceCreateResult(Structure):
    """ThryveServiceCreateResult: a service, and its context."""

    _fields_ = [("service", c_void_p), ("ctx", c_void_p)]


# The callbacks' types.
TaskJob = CFUNCTYPE(None, POINTER(TaskResult), c_void_p)
FreeFn = CFUNCTYPE(None, c_void_p)
PluginCleanUp = CFUNCTYPE(c_int, c_void_p)
PluginEntryPoint = CFUNCTYPE(None, POINTER(TaskResult), c_void_p)
ObserverCleanUp = CFUNCTYPE(None, c_void_p)
ObsJob = CFUNCTYPE(None, c_void_p)
AdvJob = CFUNCTYPE(c_void_p, c_void_p)
ThenJob = CFUNCTYPE(c_void_p, c_void_p, c_void_p)
SplitJob = CFUNCTYPE(c_void_p, c_size_t, c_size_t, c_void_p)
ChunkJob = CFUNCTYPE(c_void_p, c_void_p, c_size_t, c_void_p)

_H = c_void_p  # a handle: an opaque pointer
_STATUS = c_int  # a ThryveStatus
_HANDLES = POINTER(c_void_p)  # an array of handles

# Every function in thryve.h: its return type, then its parameters' types.
_FUNCTIONS = {
    # Errors and plain results
    "thryve_last_error": (c_char_p,),
    "thryve_task_result_new": (TaskResult, c_char_p, c_char_p, c_char_p),
    "thryve_task_result_new_with_chain": (TaskResult, c_char_p, c_char_p, c_char_p, _H),
    # Configuration, context and packet handles
    "thryve_config_default": (_H,),
    "thryve_config_from_file": (_H, c_char_p),
    "thryve_config_free": (None, _H),
    "thryve_packet_handle_free": (None, _H),
    "thryve_packet_handle_clone": (_H, _H),
    "thryve_ctx_create": (_H, _H),
    "thryve_ctx_free": (None, _H),
    "thryve_ctx_add_plugin": (_STATUS, _H, _H),
    "thryve_ctx_add_observer": (_STATUS, _H, _H),
    "thryve_ctx_set_logging_file_names": (_STATUS, _H, POINTER(c_char_p), c_int32),
    "thryve_ctx_get_packet_handle": (_H, _H),
    "thryve_packet_handle_send_task": (_STATUS, _H, _H),
    "thryve_packet_handle_send_notification": (_STATUS, _H, _H),
    # Plain tasks
    "thryve_task_create": (_H, c_char_p, c_uint8, TaskJob, c_void_p, FreeFn),
    "thryve_task_free": (None, _H),
    # Running the engine
    "thryve_run_interactive_start": (_H, _H),
    "thryve_run_interactive_free": (_STATUS, _H),
    "thryve_service_create": (ServiceCreateResult, _H),
    "thryve_service_run": (_STATUS, _H, _H),
    "thryve_service_free": (None, _H),
    # Plugins, observers and notifications
    "thryve_plugin_create": (_H, c_char_p, c_void_p, _H, PluginCleanUp, PluginEntryPoint),
    "thryve_plugin_free": (None, _H),
    "thryve_observer_create": (_H, c_char_p, c_void_p, _H, ObserverCleanUp, ObsJob),
    "thryve_observer_free": (None, _H),
    "thryve_observer_pop": (_H, _H),
    "thryve_observer_core": (c_void_p, _H),
    "thryve_notification_create": (_H, c_char_p, c_char_p, c_void_p),
    "thryve_notification_free": (None, _H),
    "thryve_notification_core": (c_void_p, _H),
    # Typed tasks
    "thryve_outcome_value": (_H, c_void_p, FreeFn),
    "thryve_outcome_error": (_H, c_int32, c_char_p),
    "thryve_outcome_forward": (_H, _H),
    "thryve_outcome_free": (None, _H),
    "thryve_adv_task_create": (_H, c_char_p, c_uint8, AdvJob, c_void_p, FreeFn),
    "thryve_adv_task_create_forward": (_H, c_char_p, c_uint8, AdvJob, c_void_p, FreeFn),
    "thryve_adv_task_free": (None, _H),
    "thryve_packet_handle_send_adv_task": (_H, _H, _H),
    "thryve_adv_handle_then": (_H, _H, c_char_p, ThenJob, c_void_p, FreeFn),
    "thryve_adv_handle_then_with_priority": (_H, _H, c_char_p, c_uint8, ThenJob, c_void_p, FreeFn),
    "thryve_adv_handle_then_forward": (_H, _H, c_char_p, ThenJob, c_void_p, FreeFn),
    "thryve_adv_handle_cancel": (None, _H),
    "thryve_adv_handle_free": (None, _H),
    "thryve_adv_handles_join": (_H, _HANDLES, c_size_t, c_char_p, ThenJob, c_void_p, FreeFn),
    "thryve_adv_handles_join_with_priority": (
        _H, _HANDLES, c_size_t, c_char_p, c_uint8, ThenJob, c_void_p, FreeFn,
    ),
    "thryve_adv_handles_cancel_all": (None, _HANDLES, c_size_t),
    "thryve_packet_handle_split_into": (
        _STATUS, _H, c_size_t, c_char_p, c_size_t, c_uint8, SplitJob, c_void_p, FreeFn, _HANDLES,
        POINTER(c_size_t),
    ),
    "thryve_packet_handle_send_split_tasks": (
        _STATUS, _H, _HANDLES, c_size_t, FreeFn, c_char_p, c_uint8, ChunkJob, c_void_p, FreeFn,
        _HANDLES,
    ),
    "thryve_adv_handle_fan_out": (_H, _H),
    "thryve_fanout_then": (_H, _H, c_char_p, ThenJob, c_void_p, FreeFn),
    "thryve_fanout_then_with_priority": (_H, _H, c_char_p, c_uint8, ThenJob, c_void_p, FreeFn),
    "thryve_fanout_send": (_STATUS, _H),
    "thryve_handoff_is_error": (c_bool, _H),
    "thryve_handoff_value_count": (c_size_t, _H),
    "thryve_handoff_value": (c_void_p, _H, c_size_t),
    "thryve_handoff_take_value": (c_void_p, _H, c_size_t),
    "thryve_handoff_error_code": (c_int32, _H),
    "thryve_handoff_error_message": (c_char_p, _H),
    "thryve_handoff_error_position": (c_size_t, _H),
}

# The library's file name on this platform.
_LIBRARY = {"win32": "thryve_ffi.dll", "darwin": "libthryve_ffi.dylib"}.get(
    sys.platform, "libthryve_ffi.so"
)

# The C library, once load() has opened it.
lib = None


def _places(path):
    """Where the library may be: `path`, else THRYVE_FFI, else this repository's build."""
    if path is None:
        path = os.environ.get("THRYVE_FFI")
    if path is None:
        # thryve.py is in examples/python/: the repository is two folders up. A copy elsewhere
        # finds no build there, and says so.
        target = Path(__file__).resolve().parent.parent.parent / "target"
        return [target / "debug" / _LIBRARY, target / "release" / _LIBRARY]

    # A folder: the library's own, or a release's, unzipped (bin/ on Windows, lib/ elsewhere).
    path = Path(path)
    if path.is_dir():
        return [path / _LIBRARY, path / "bin" / _LIBRARY, path / "lib" / _LIBRARY]
    return [path]


def load(path=None):
    """Opens Thryve's C library, once, and returns `lib`.

    The library is at `path` if it's given, else at the THRYVE_FFI environment variable, else in
    this repository's build (target/debug, then target/release). Either can also name a folder:
    the library's, or a release's, unzipped.
    """
    global lib
    if lib is not None:
        return lib

    places = _places(path)
    for place in places:
        if place.is_file():
            library = ctypes.CDLL(str(place))
            for name, (restype, *argtypes) in _FUNCTIONS.items():
                function = getattr(library, name)
                function.restype = restype
                function.argtypes = argtypes
            lib = library
            return lib

    raise Error(
        "Thryve's C library isn't in "
        + ", ".join(str(place) for place in places)
        + ". Build it with `cargo build -p thryve_ffi`, or set THRYVE_FFI to a release's folder."
    )


def last_error():
    """Why the latest call to Thryve on this thread failed."""
    error = lib.thryve_last_error()
    return error.decode(errors="replace") if error else "unknown error"


def _check(status):
    """Raises Error, with Thryve's reason, unless `status` is OK."""
    if status != OK:
        raise Error(last_error())


def _made(handle):
    """Returns the handle a call made; raises Error, with Thryve's reason, if it made none."""
    if not handle:
        raise Error(last_error())
    return handle


# ============================================================================
# Python objects, held for Thryve
# ============================================================================

_held = {}
_ids = itertools.count(1)
_ids_lock = threading.Lock()


def hold(obj):
    """Holds a Python object for Thryve, and returns the id Thryve carries in its place."""
    with _ids_lock:
        key = next(_ids)
    _held[key] = obj
    return key


def let_go(key):
    """Stops holding the object under an id, and returns it."""
    return _held.pop(key, None)


@FreeFn
def free_held(key):
    """The free function for held objects: Thryve calls it once it's done with an id."""
    if key:
        _held.pop(key, None)


def _outcome(job, *args):
    """Runs a typed task's job, and makes what it returns, or raises, its outcome."""
    try:
        return lib.thryve_outcome_value(hold(job(*args)), free_held)
    except TaskError as error:
        return lib.thryve_outcome_error(error.code, str(error.message).encode())
    except Exception as error:
        return lib.thryve_outcome_error(1, (str(error) or type(error).__name__).encode())


class _Reporter:
    """A plain job, and the C strings of its latest result, which Thryve reads after it returns."""

    __slots__ = ("job", "clean_up", "strings")

    def __init__(self, job, clean_up=None):
        self.job = job
        self.clean_up = clean_up
        self.strings = None


def _report(result, reporter):
    """Runs a plain job, and fills in its result. An exception, or a result that isn't
    (result_type, message, log_file) or None, is reported in Error.log."""
    try:
        answer = reporter.job()
        if answer is not None:
            result_type, message, log_file = answer
            answer = (result_type, message, log_file)
    except Exception as error:
        traceback.print_exc()
        answer = ("Error", f"{type(error).__name__}: {error}", "Error")
    if answer is not None:
        reporter.strings = tuple(str(part).encode() for part in answer)
        result[0] = lib.thryve_task_result_new(*reporter.strings)


# The callbacks Thryve calls: each finds its Python job by the id it's given.


@TaskJob
def _task_job(result, key):
    _report(result, _held[key])


@PluginEntryPoint
def _entry_point(result, key):
    _report(result, _held[key])


@PluginCleanUp
def _plugin_clean_up(key):
    # An exception mustn't escape: ctypes would hand Thryve an arbitrary status in its place.
    reporter = _held.get(key)
    if reporter is not None and reporter.clean_up is not None:
        try:
            reporter.clean_up()
        except Exception:
            traceback.print_exc()
            return OPERATION_FAIL
    return OK


@ObsJob
def _observer_job(observer):
    reporter = _held[lib.thryve_observer_core(observer)]
    try:
        reporter.job(_notifications(observer))
    except Exception:
        traceback.print_exc()


@ObserverCleanUp
def _observer_clean_up(key):
    reporter = _held.get(key)
    if reporter is not None and reporter.clean_up is not None:
        try:
            reporter.clean_up()
        except Exception:
            traceback.print_exc()


@AdvJob
def _adv_job(key):
    return _outcome(_held[key])


@ThenJob
def _then_job(handoff, key):
    return _outcome(_held[key], Handoff(handoff))


@SplitJob
def _split_job(first, count, key):
    return _outcome(_held[key], first, count)


@ChunkJob
def _chunk_job(chunk, index, key):
    return _outcome(_held[key], _held.get(chunk), index)


class Handoff:
    """What a follow-up's job gets: the values of the tasks it follows, or the error of the one
    that failed. It's valid only while the job runs."""

    def __init__(self, handoff):
        self._handoff = handoff

    @property
    def error(self):
        """The error of the task that failed, as a TaskError; None if none did."""
        if not lib.thryve_handoff_is_error(self._handoff):
            return None
        message = lib.thryve_handoff_error_message(self._handoff)
        return TaskError(
            lib.thryve_handoff_error_code(self._handoff),
            message.decode(errors="replace") if message else "",
            lib.thryve_handoff_error_position(self._handoff),
        )

    @property
    def values(self):
        """The values, in order: one for a follow-up or a fan-out follower, one per task for a
        join. A fan-out's followers share theirs, so none may change it."""
        count = lib.thryve_handoff_value_count(self._handoff)
        return [_held.get(lib.thryve_handoff_value(self._handoff, i)) for i in range(count)]

    @property
    def value(self):
        """The one value of a follow-up or a fan-out follower."""
        return self.values[0]


def _notifications(observer):
    """The data of each notification sent to an observer, as it comes, until the engine stops."""
    while True:
        notif = lib.thryve_observer_pop(observer)
        if not notif:
            return
        key = lib.thryve_notification_core(notif)
        lib.thryve_notification_free(notif)
        yield let_go(key) if key else None


# ============================================================================
# The calls, for Python
# ============================================================================


def config_from_file(path, root_dir=None):
    """The configuration in the TOML file at `path`.

    With `root_dir`, Thryve's root folder, where its logs and graph go, is that folder rather than
    the program's, which for Python is Python's own. The C library has no setter for it: a copy of
    the file, with `root_dir` set, is written into that folder and loaded."""
    if root_dir is not None:
        root = Path(root_dir).resolve()
        root.mkdir(parents=True, exist_ok=True)
        text, found = re.subn(
            r"^root_dir\s*=.*$",
            lambda _: f"root_dir = {json.dumps(str(root))}",
            Path(path).read_text(encoding="utf-8"),
            count=1,
            flags=re.MULTILINE,
        )
        if not found:
            raise Error(f"{path} has no root_dir")
        path = root / "thryve.toml"
        path.write_text(text, encoding="utf-8")
    return _made(lib.thryve_config_from_file(str(path).encode()))


def ctx_create(config):
    """A context: what the engine is built from. The configuration stays the caller's."""
    return _made(lib.thryve_ctx_create(config))


def ctx_get_packet_handle(ctx):
    """The engine's packet handle, which everything sends with; the caller frees it."""
    return _made(lib.thryve_ctx_get_packet_handle(ctx))


def plugin(plugin_type, tx, start, clean_up=None):
    """A plugin. `start` is its entry point: it runs once, on a worker, when the engine starts,
    and returns its result as (result_type, message, log_file). `clean_up` runs when the engine
    stops."""
    key = hold(_Reporter(start, clean_up))
    handle = lib.thryve_plugin_create(
        plugin_type.encode(), key, tx, _plugin_clean_up, _entry_point
    )
    if not handle:
        let_go(key)
        raise Error(last_error())
    return handle


def observer(observer_type, tx, run, clean_up=None):
    """An observer. `run` is its job: it runs once, on the observer's own thread, with the data of
    the notifications sent to it, to iterate over until the engine stops. `clean_up` runs when the
    engine stops."""
    key = hold(_Reporter(run, clean_up))
    handle = lib.thryve_observer_create(
        observer_type.encode(), key, tx, _observer_clean_up, _observer_job
    )
    if not handle:
        let_go(key)
        raise Error(last_error())
    return handle


def ctx_add_plugin(ctx, plugin_handle):
    """Adds a plugin to the context, which owns it from then on. If it can't, frees it."""
    if lib.thryve_ctx_add_plugin(ctx, plugin_handle) != OK:
        error = Error(last_error())
        lib.thryve_plugin_free(plugin_handle)
        raise error


def ctx_add_observer(ctx, observer_handle):
    """Adds an observer to the context, which owns it from then on. If it can't, frees it."""
    if lib.thryve_ctx_add_observer(ctx, observer_handle) != OK:
        error = Error(last_error())
        lib.thryve_observer_free(observer_handle)
        raise error


def run_interactive(ctx):
    """Starts the engine, and waits until a task with a Fin or FinErr result stops it. The context
    is used up."""
    run = _made(lib.thryve_run_interactive_start(ctx))
    _check(lib.thryve_run_interactive_free(run))


def send_task(tx, name, priority, job):
    """Sends a plain task, whose `job` returns its result as (result_type, message, log_file), or
    None for none."""
    task = _made(lib.thryve_task_create(name.encode(), priority, _task_job, hold(_Reporter(job)),
                                        free_held))
    _check(lib.thryve_packet_handle_send_task(tx, task))


def send_notification(tx, notif_type, receiver, data):
    """Sends `data` to the observers of type `receiver`, as a notification of `notif_type`."""
    key = hold(data)
    notif = lib.thryve_notification_create(notif_type.encode(), receiver.encode(), key)
    if not notif or lib.thryve_packet_handle_send_notification(tx, notif) != OK:
        error = Error(last_error())
        let_go(key)
        raise error


def send_adv_task(tx, name, priority, job):
    """Sends a typed task, whose `job()` returns its value. Returns its handle."""
    task = _made(lib.thryve_adv_task_create(name.encode(), priority, _adv_job, hold(job),
                                            free_held))
    return _made(lib.thryve_packet_handle_send_adv_task(tx, task))


def then(handle, name, job):
    """Declares a follow-up: `job(handoff)` runs once the task is done. Uses up the handle, and
    returns the follow-up's."""
    return _made(lib.thryve_adv_handle_then(handle, name.encode(), _then_job, hold(job), free_held))


def split_into(tx, item_count, name, pieces, priority, job):
    """Cuts `item_count` items into at most `pieces` pieces, in order, and sends a typed task for
    each: `job(first, count)` with where its items start and how many it has. Returns their
    handles."""
    handles = (c_void_p * pieces)()
    count = c_size_t()
    _check(lib.thryve_packet_handle_split_into(
        tx, item_count, name.encode(), pieces, priority, _split_job, hold(job), free_held, handles,
        byref(count),
    ))
    return list(handles[: count.value])


def join(handles, name, job):
    """Joins typed tasks: `job(handoff)` runs once all of them are done, with their values in this
    order, or as soon as one fails, with its error. Uses up the handles, and returns the join's."""
    array = (c_void_p * len(handles))(*handles)
    return _made(lib.thryve_adv_handles_join(
        array, len(handles), name.encode(), _then_job, hold(job), free_held
    ))


def fan_out(handle):
    """Starts a fan-out: followers declared with fanout_then share the task's value. Uses up the
    handle, and returns the group."""
    return _made(lib.thryve_adv_handle_fan_out(handle))


def fanout_then(group, name, job):
    """Declares a fan-out follower: `job(handoff)` runs once the task is done. Returns its handle."""
    return _made(lib.thryve_fanout_then(group, name.encode(), _then_job, hold(job), free_held))


def fanout_send(group):
    """Sends a fan-out's followers. Uses up the group."""
    _check(lib.thryve_fanout_send(group))


def free_handle(handle):
    """Lets a typed task's handle go, when nothing follows it."""
    lib.thryve_adv_handle_free(handle)
