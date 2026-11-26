// SPDX-License-Identifier: GPL-3.0-or-later
use std::process::ExitCode;

fn main() -> ExitCode {
    switch_installer_lib::run_from_env()
}
