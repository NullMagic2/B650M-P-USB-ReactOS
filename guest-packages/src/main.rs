//! Module: lightning.guest_packages.cli
//! Purpose: Expose the same package planner used by the manager to build automation.
//! Created: 2026-10-25
//! Architecture: No guest or host device mutation; list and prepare share the library.
use anyhow::{ensure, Result};
use std::{env, path::Path};
fn run() -> Result<()> {
    let args: Vec<_> = env::args().skip(1).collect();
    ensure!(
        !args.is_empty(),
        "Usage: lightning-guest-packages list ROOT | prepare or prepare-exe ROOT SETUP OUTPUT PACKAGE..."
    );
    match args[0].as_str() {
        "list" => {
            ensure!(args.len() == 2, "list requires ROOT");
            println!(
                "{}",
                serde_json::to_string_pretty(
                    &lightning_guest_packages::Catalog::load(Path::new(&args[1]))?.choices()
                )?
            );
        }
        "prepare" | "prepare-exe" => {
            ensure!(
                args.len() >= 5,
                "prepare requires ROOT SETUP OUTPUT PACKAGE..."
            );
            let prepare = if args[0] == "prepare-exe" {
                lightning_guest_packages::prepare_executable
            } else {
                lightning_guest_packages::prepare
            };
            prepare(
                Path::new(&args[1]),
                &args[4..],
                Path::new(&args[2]),
                Path::new(&args[3]),
            )?;
        }
        _ => anyhow::bail!("Unknown guest package operation"),
    }
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("Guest packages: {error:#}");
        std::process::exit(1);
    }
}
