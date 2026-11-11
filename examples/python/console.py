# Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
#
# This file is part of Thryve, dual-licensed under the GNU Affero General
# Public License v3.0 (or later) and a separate Commercial License.
# See LICENSE and COMMERCIAL.md in the repository root, or visit
# https://thryve-framework.com for licensing options.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Console: an observer that prints the projects' reports, then ends the run.

Each project sends its report as a notification. Console keeps the reports until every project's
is in, and prints them in the order it was given the projects: the output is the same on every run,
whichever project finishes first.
"""

import thryve
from project_report import ProjectReport


def shut_down():
    """The task that ends the run: a Fin result stops the engine."""
    return "Fin", "the reports are out", "Main"


class Console:
    """An observer that prints the projects' reports once all of them are in, then ends the run."""

    def __init__(self, projects, tx):
        self.projects = projects  # the projects' names, in the order their reports are printed
        self.tx = tx

    def run(self, notifications):
        """The observer's job: its whole loop, run once, on its own thread. The notifications come
        as they're sent, and stop once the engine does."""
        # Each project's report, kept until all of them are in.
        reports = {}
        for report in notifications:
            if not isinstance(report, ProjectReport) or report.project not in self.projects:
                continue
            reports[report.project] = report.text
            if len(reports) < len(self.projects):
                continue

            # Every report is in: they're printed a blank line apart.
            print("\n".join(reports[project] for project in self.projects), end="", flush=True)
            reports = {}

            # The reports are out: a task with a Fin result ends the run.
            try:
                thryve.send_task(self.tx, "Shutdown", 0, shut_down)
            except thryve.Error:
                pass


def observer(projects, tx):
    """The Console observer, for `projects`: it prints their reports in this order."""
    return thryve.observer("Console", tx, Console(projects, tx).run)
