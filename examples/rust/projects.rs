// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

//! Two projects in one engine: the access-log report and Primes.
//!
//! Each project is a plugin of its own, and neither names the other. The engine runs both at the
//! same time, on the same workers; when tasks of both wait for a worker, the report's go first,
//! since they have the higher priority. Console prints the reports once all of them are in, in the
//! order the projects were named, then ends the run.
//!
//! Name the projects to run, or none for both. From anywhere in the repository:
//!
//! ```text
//! cargo run -p thryve_examples --bin projects [access-log] [primes]
//! ```

use std::path::Path;
use std::process::ExitCode;

use thryve::prelude::*;
use thryve::run::run_interactive;
use thryve::utils::bases::plugin::Plugin;
use thryve::utils::cfg::Config;
use thryve::utils::error::get_err;
use thryve_examples::console::Console;
use thryve_examples::log_report::{self, LogReader};
use thryve_examples::primes::{self, Primes};

/// Makes a project's plugin, from the folder of shared files and the engine's packet handle.
type MakePlugin = fn(&Path, PacketHandle) -> Plugin;

/// The projects this program knows, by the names it's run with. Each one is a plugin: adding a
/// project to the engine is one more line here.
const PROJECTS: [(&str, MakePlugin); 2] = [
    (log_report::PROJECT, |data, tx| {
        LogReader::plugin(data.join("access.log"), tx)
    }),
    (primes::PROJECT, |_data, tx| Primes::plugin(tx)),
];

fn main() -> ExitCode {
    // The projects named on the command line, each once, in the order given; all of them when none
    // is named.
    let mut chosen: Vec<(&'static str, MakePlugin)> = Vec::new();
    for name in std::env::args().skip(1) {
        let Some(&project) = PROJECTS.iter().find(|(project, _)| *project == name) else {
            let known: Vec<&str> = PROJECTS.iter().map(|(project, _)| *project).collect();
            eprintln!(
                "unknown project: {name}. The projects are: {}",
                known.join(", ")
            );
            return ExitCode::from(2);
        };
        if !chosen.iter().any(|(already, _)| *already == project.0) {
            chosen.push(project);
        }
    }
    if chosen.is_empty() {
        chosen = PROJECTS.to_vec();
    }
    let names: Vec<&'static str> = chosen.iter().map(|(name, _)| *name).collect();

    // The files every example shares: the sample log, and Thryve's configuration.
    let data = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("data");
    let config = match Config::load_config_from_file(data.join("thryve.toml")) {
        Ok(config) => config,
        Err(error) => {
            eprintln!("{}", get_err(&error));
            return ExitCode::FAILURE;
        }
    };

    println!("Projects in this engine: {}", names.join(", "));

    // Each project is a plugin of its own. Console prints their reports, in the order named.
    let mut ctx = run_interactive::summon_requirements(config);
    for (_, make_plugin) in &chosen {
        ctx.plugins.push(make_plugin(&data, ctx.packet_handle()));
    }
    ctx.observers
        .push(Console::observer(names, ctx.packet_handle()));

    // The engine runs until Console's `Fin` task, or a project's `FinErr`, ends it.
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
