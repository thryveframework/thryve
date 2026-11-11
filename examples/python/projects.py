# Thryve — Copyright (c) 2026 Youssef Mostafa Abdellatif
#
# This file is part of Thryve, dual-licensed under the GNU Affero General
# Public License v3.0 (or later) and a separate Commercial License.
# See LICENSE and COMMERCIAL.md in the repository root, or visit
# https://thryve-framework.com for licensing options.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Two projects in one engine, in Python: the access-log report and Primes.

Each project is a plugin of its own, and neither names the other. The engine runs both at the same
time, on the same workers; when tasks of both wait for a worker, the report's go first, since they
have the higher priority. Console prints the reports once all of them are in, in the order the
projects were named, then ends the run.

Name the projects to run, or none for both. From anywhere:

    python examples/python/projects.py [access-log] [primes]

It needs Thryve's C library, like access_log.py. The Rust, C and C++ versions in examples/ print
the same.
"""

import sys
from pathlib import Path

import console
import log_report
import primes
import thryve

# The projects this program knows, by the names it's run with, and how each one's plugin is made
# from the examples folder and the engine's packet handle. Adding a project to the engine is one
# more line here.
PROJECTS = {
    log_report.PROJECT: lambda examples, tx: log_report.plugin(
        examples / "data" / "access.log", tx
    ),
    primes.PROJECT: lambda examples, tx: primes.plugin(tx),
}


def main():
    # The projects named on the command line, each once, in the order given; all of them when none
    # is named.
    chosen = []
    for name in sys.argv[1:]:
        if name not in PROJECTS:
            known = ", ".join(PROJECTS)
            print(f"unknown project: {name}. The projects are: {known}", file=sys.stderr)
            return 2
        if name not in chosen:
            chosen.append(name)
    chosen = chosen or list(PROJECTS)

    # The files every example shares: the sample log, and Thryve's configuration. Thryve's root
    # folder is python/out: the program's own folder is Python's.
    examples = Path(__file__).resolve().parent.parent

    try:
        thryve.load()
        config = thryve.config_from_file(
            examples / "data" / "thryve.toml", root_dir=examples / "python" / "out"
        )
        print(f"Projects in this engine: {', '.join(chosen)}", flush=True)
        ctx = thryve.ctx_create(config)
        tx = thryve.ctx_get_packet_handle(ctx)

        # Each project is a plugin of its own. Console prints their reports, in the order named.
        for name in chosen:
            thryve.ctx_add_plugin(ctx, PROJECTS[name](examples, tx))
        thryve.ctx_add_observer(ctx, console.observer(chosen, tx))

        # The engine runs until Console's Fin task, or a project's FinErr, ends it. Running it
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
