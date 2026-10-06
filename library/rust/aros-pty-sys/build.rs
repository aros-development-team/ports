fn main() {
    println!("cargo:rerun-if-changed=src/pty.c");
    println!("cargo:rerun-if-changed=src/pty.h");

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("aros") {
        return;
    }

    // AROS executables are relocatable objects: no PIC, which cc adds by default.
    cc::Build::new().file("src/pty.c").pic(false).compile("aros_pty");
}
