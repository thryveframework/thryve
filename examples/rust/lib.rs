// Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
//
// This file is part of Thryve, dual-licensed under the GNU Affero General
// Public License v3.0 (or later) and a separate Commercial License.
// See LICENSE and COMMERCIAL.md in the repository root, or visit
// https://thryve-framework.com for licensing options.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

//! What Thryve's Rust examples are made of. Each example is a program that runs one or more
//! projects in one engine:
//!
//! - `access_log` runs the access-log report ([`log_report`]);
//! - `projects` runs the report and [`primes`] together, or either one.
//!
//! Both print the reports with [`console`].

use thryve::prelude::*;
use thryve::utils::bases::notif::Notif;
use thryve::utils::impl_as_any;
use thryve::utils::traits::NotifLogic;

pub mod console;
pub mod log_report;
pub mod primes;

/// A project's report, on its way to Console as a notification. It's all the projects and Console
/// share: neither names the other.
pub(crate) struct ProjectReport {
    /// The project's name, as the programs know it.
    pub(crate) project: &'static str,
    /// The report.
    pub(crate) text: String,
}

impl NotifLogic for ProjectReport {
    impl_as_any!(ProjectReport);
}

/// Sends `project`'s report to Console, as a notification.
pub(crate) fn send_report(tx: &PacketHandle, project: &'static str, text: String) {
    let report = Notif::new(
        "Report",
        "Console",
        Box::new(ProjectReport { project, text }),
    )
    .expect("the notification's names are valid");
    // Sending fails only once the engine is gone, and then there's nobody left to tell.
    let _ = tx.send_notification(report);
}
