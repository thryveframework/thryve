# Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
#
# This file is part of Thryve, dual-licensed under the GNU Affero General
# Public License v3.0 (or later) and a separate Commercial License.
# See LICENSE and COMMERCIAL.md in the repository root, or visit
# https://thryve-framework.com for licensing options.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A project's report, on its way to Console as a notification. It's all the projects and Console
share: neither names the other."""

import thryve


class ProjectReport:
    """A project's report: the project's name, as the programs know it, and the report."""

    def __init__(self, project, text):
        self.project = project
        self.text = text


def send_report(tx, project, text):
    """Sends `project`'s report to Console, as a notification."""
    # Sending fails only once the engine is gone, and then there's nobody left to tell.
    try:
        thryve.send_notification(tx, "Report", "Console", ProjectReport(project, text))
    except thryve.Error:
        pass
