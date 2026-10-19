//! Compiles the C side of the shell (`c/aros_extra.c`) with the AROS C cross compiler and links
//! it in. The compiler and archiver come from `AROS_CC` / `AROS_AR` (the AROS build system sets
//! them), else `<arch>-aros-cc` / `<arch>-aros-ar` on the PATH (what the Rust crosstool installs).
//! On any other target the file is skipped, so the workspace still type-checks on the host.

use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=c/aros_extra.c");
    println!("cargo:rerun-if-env-changed=AROS_CC");
    println!("cargo:rerun-if-env-changed=AROS_AR");
    let os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if os != "aros" {
        return;
    }
    let arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    let cc = std::env::var("AROS_CC").unwrap_or_else(|_| format!("{arch}-aros-cc"));
    let ar = std::env::var("AROS_AR").unwrap_or_else(|_| format!("{arch}-aros-ar"));
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap_or_default());
    let obj = out.join("aros_extra.o");
    let lib = out.join("libsolidcraft_aros_extra.a");
    let status = Command::new(&cc).args(["-O2", "-Wall", "-c", "c/aros_extra.c", "-o"]).arg(&obj).status();
    match status {
        Ok(s) if s.success() => {}
        Ok(s) => fail(&format!("{cc} failed with {s}")),
        Err(e) => fail(&format!("cannot run {cc}: {e}")),
    }
    let _ = std::fs::remove_file(&lib);
    let status = Command::new(&ar).arg("crs").arg(&lib).arg(&obj).status();
    match status {
        Ok(s) if s.success() => {}
        Ok(s) => fail(&format!("{ar} failed with {s}")),
        Err(e) => fail(&format!("cannot run {ar}: {e}")),
    }
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=solidcraft_aros_extra");
}

/// A build script reports its failure through its exit status.
fn fail(msg: &str) -> ! {
    eprintln!("solidcraft-aros build.rs: {msg}");
    std::process::exit(1);
}
