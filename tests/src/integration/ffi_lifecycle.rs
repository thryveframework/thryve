// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

//! Full FFI lifecycle integration test: create -> run -> send task through the
//! FFI boundary -> await shutdown -> free.
//!
//! `thryve_tests::tests_ffi::lifecycle` deliberately stops short of running the
//! engine (see its `test_full_interactive_setup`, which "verifies the full
//! interactive setup flow without actually running the engine (which would
//! block)"). This test is the other half: it actually calls
//! `thryve_run_interactive_start`, sends a task through the real FFI packet
//! handle, and blocks on `thryve_run_interactive_free` until the engine drains
//! and exits - the same sequence a real C/C++ consumer (see
//! `examples/c_cpp/c/`) drives through the extern "C" boundary.

mod tests_ffi_full_lifecycle {
    use std::ffi::CString;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use thryve_ffi::{
        ThryveStatus, ThryveTaskResult, thryve_config_default, thryve_config_free,
        thryve_ctx_create, thryve_ctx_get_packet_handle, thryve_packet_handle_free,
        thryve_packet_handle_send_task, thryve_run_interactive_free, thryve_run_interactive_start,
        thryve_task_create, thryve_task_result_new,
    };

    static TASK_RAN: AtomicUsize = AtomicUsize::new(0);

    extern "C" fn finishing_job(result: *mut ThryveTaskResult, _user_data: *mut std::ffi::c_void) {
        TASK_RAN.fetch_add(1, Ordering::SeqCst);
        // "Fin" is the result type the engine shuts down on.
        //
        // C string literals, not `CString::new(...).unwrap()`: Thryve reads these pointers after
        // the job returns, when a CString local to the job would already be freed. A dangling
        // read here once made this test hang: the engine never saw "Fin" and looped forever.
        unsafe {
            *result =
                thryve_task_result_new(c"Fin".as_ptr(), c"done via FFI".as_ptr(), c"Main".as_ptr())
        };
    }

    #[test]
    fn test_full_ffi_run_interactive_lifecycle_processes_task_and_exits() {
        TASK_RAN.store(0, Ordering::SeqCst);

        // Arrange
        let config = thryve_config_default();
        let ctx = thryve_ctx_create(config);
        let pkt = thryve_ctx_get_packet_handle(ctx);
        assert!(!pkt.is_null(), "packet handle should not be null");

        // Act - start the engine on a background thread. This consumes `ctx`;
        // it must NOT be freed separately afterward (see the doc comment on
        // thryve_run_interactive_start).
        //
        // thryve_run_interactive_start runs run_interactive's startup under the
        // hood, which calls init_panic_hook() - a process-wide hook with no
        // test-scoping that calls std::process::exit(1) on ANY panic anywhere
        // in this process (see engine_lifecycle.rs for the same note). Left
        // in place, it silently turns every #[should_panic] test that runs
        // afterward into a hard process crash instead of a caught unwind.
        // Save and restore it around the engine's lifetime to keep that
        // side effect contained.
        let prev_hook = std::panic::take_hook();

        let run_handle = thryve_run_interactive_start(ctx);
        assert!(
            !run_handle.is_null(),
            "thryve_run_interactive_start should return a handle"
        );

        let task = unsafe {
            thryve_task_create(
                CString::new("ThryveShutdown").unwrap().as_ptr(),
                0,
                Some(finishing_job),
                std::ptr::null_mut(),
                None,
            )
        };
        assert!(!task.is_null(), "task should not be null");

        let send_status = thryve_packet_handle_send_task(pkt, task);
        assert_eq!(send_status, ThryveStatus::Ok);

        // Assert - a successful join proves the engine actually received and
        // processed the task through the real FFI boundary and exited on Fin;
        // if the task were dropped or the engine never drained, this blocks
        // forever instead of returning.
        let join_status = thryve_run_interactive_free(run_handle);

        std::panic::set_hook(prev_hook);

        assert_eq!(join_status, ThryveStatus::Ok);
        assert_eq!(
            TASK_RAN.load(Ordering::SeqCst),
            1,
            "the FFI job should have run exactly once"
        );

        // Cleanup - ctx was consumed by thryve_run_interactive_start above.
        thryve_packet_handle_free(pkt);
        thryve_config_free(config);
    }
}
