// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

//! Console: an observer that prints the projects' reports, then ends the run.
//!
//! Each project sends its report as a notification. Console keeps the reports until every project's
//! is in, and prints them in the order it was given the projects: the output is the same on every
//! run, whichever project finishes first.

use std::sync::Arc;

use thryve::prelude::*;
use thryve::utils::bases::observer::Observer;
use thryve::utils::impl_as_any;
use thryve::utils::jobs::ObsJob;
use thryve::utils::packets::task::Task;
use thryve::utils::res::TaskResult;
use thryve::utils::traits::{ObsLogic, Queue};

use crate::ProjectReport;

/// Console: an observer that prints the projects' reports once all of them are in, then ends the
/// run.
pub struct Console;

impl Console {
    /// The Console observer, for `projects`: it prints their reports in this order.
    pub fn observer(projects: Vec<&'static str>, tx: PacketHandle) -> Observer {
        let sender = tx.clone();
        let job = ObsJob::new(move |observer: Arc<Observer>| {
            // Each project's report, in the projects' order, kept until all of them are in.
            let mut reports: Vec<Option<String>> = vec![None; projects.len()];

            // `pop` waits for the next notification, and returns `None` once the engine stops.
            while let Some(notif) = observer.obs_queue.pop() {
                let Some(report) = notif.notif_core.as_any().downcast_ref::<ProjectReport>() else {
                    continue;
                };
                let Some(place) = projects
                    .iter()
                    .position(|project| *project == report.project)
                else {
                    continue;
                };
                reports[place] = Some(report.text.clone());
                if reports.contains(&None) {
                    continue;
                }

                // Every report is in: they're printed a blank line apart.
                let texts: Vec<String> = reports.iter_mut().filter_map(Option::take).collect();
                print!("{}", texts.join("\n"));

                // The reports are out: a task with a `Fin` result ends the run.
                let fin = Task::new("Shutdown", 0, || {
                    TaskResult::new("Fin", String::from("the reports are out"), "Main")
                })
                .expect("the task's name is valid");
                let _ = sender.send_task(fin);
            }
        });
        Observer::new("Console", Arc::new(Self), job, tx).expect("the observer's type is valid")
    }
}

impl ObsLogic for Console {
    fn clean_up(&self, _observer: Arc<Observer>) {}

    impl_as_any!(Console);
}
