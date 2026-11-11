// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

//! The access-log report on its own: Thryve reads a web server's access log and reports on it.
//!
//! The report's steps are in `log_report.rs`. Console, the observer that prints the report and ends
//! the run, is in `console.rs`.
//!
//! Run it from anywhere in the repository; the log defaults to `examples/data/access.log`:
//!
//! ```text
//! cargo run -p thryve_examples --bin access_log [log]
//! ```
//!
//! The C, C++ and Python versions in `examples/` print the same report from the same log.

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use thryve::run::run_interactive;
use thryve::utils::cfg::Config;
use thryve::utils::error::get_err;
use thryve_examples::console::Console;
use thryve_examples::log_report::{self, LogReader};

fn main() -> ExitCode {
    // The files every example shares: the sample log, and Thryve's configuration.
    let data = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("data");
    let log = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| data.join("access.log"));
    let config = match Config::load_config_from_file(data.join("thryve.toml")) {
        Ok(config) => config,
        Err(error) => {
            eprintln!("{}", get_err(&error));
            return ExitCode::FAILURE;
        }
    };

    // LogReader starts the work and Console shows the result: neither knows the other.
    let mut ctx = run_interactive::summon_requirements(config);
    ctx.plugins
        .push(LogReader::plugin(log, ctx.packet_handle()));
    ctx.observers.push(Console::observer(
        vec![log_report::PROJECT],
        ctx.packet_handle(),
    ));

    // The engine runs until Console's `Fin` task, or LogReader's `FinErr`, ends it.
    match run_interactive::run_interactive(ctx) {
        Ok(engine) => {
            let _ = engine.join();
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("{}", get_err(&error));
            ExitCode::FAILURE
        }
    }
}
