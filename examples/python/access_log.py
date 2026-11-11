# Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
#
# This file is part of Thryve, dual-licensed under the GNU Affero General
# Public License v3.0 (or later) and a separate Commercial License.
# See LICENSE and COMMERCIAL.md in the repository root, or visit
# https://thryve-framework.com for licensing options.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The access-log report on its own, in Python: Thryve reads a web server's access log and reports
on it.

The report's steps are in log_report.py. Console, the observer that prints the report and ends the
run, is in console.py.

Run it from anywhere; the log defaults to examples/data/access.log:

    python examples/python/access_log.py [log]

It needs Thryve's C library, through thryve.py next to it: built from this repository with
`cargo build -p thryve_ffi`, or from a release, unzipped, with THRYVE_FFI set to its folder.

The Rust, C and C++ versions in examples/ print the same report from the same log.
"""

import sys
from pathlib import Path

import console
import log_report
import thryve


def main():
    # The files every example shares: the sample log, and Thryve's configuration. Thryve's root
    # folder is python/out: the program's own folder is Python's.
    examples = Path(__file__).resolve().parent.parent
    log = Path(sys.argv[1]) if len(sys.argv) > 1 else examples / "data" / "access.log"

    try:
        thryve.load()
        config = thryve.config_from_file(
            examples / "data" / "thryve.toml", root_dir=examples / "python" / "out"
        )
        ctx = thryve.ctx_create(config)
        tx = thryve.ctx_get_packet_handle(ctx)

        # LogReader starts the work and Console shows the result: neither knows the other.
        thryve.ctx_add_plugin(ctx, log_report.plugin(log, tx))
        thryve.ctx_add_observer(ctx, console.observer([log_report.PROJECT], tx))

        # The engine runs until Console's Fin task, or LogReader's FinErr, ends it. Running it
        # uses up the context.
        thryve.run_interactive(ctx)
    except thryve.Error as error:
        print(error, file=sys.stderr)
        return 1

    thryve.lib.thryve_packet_handle_free(tx)
    thryve.lib.thryve_config_free(config)
    return 0


if __name__ == "__main__":
    sys.exit(main())
