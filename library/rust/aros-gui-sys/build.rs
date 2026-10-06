fn main() {
    println!("cargo:rerun-if-changed=src/glue.c");
    println!("cargo:rerun-if-changed=src/gl.c");
    println!("cargo:rerun-if-changed=src/clip.c");
    println!("cargo:rerun-if-changed=src/glue.h");

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("aros") {
        return;
    }

    // AROS executables are relocatable objects: no PIC, which cc adds by default.
    cc::Build::new().file("src/glue.c").file("src/gl.c").file("src/clip.c").pic(false).compile("aros_gui_glue");

    // libGL's autoinit object is only pulled in by programs that call into gl.c.
    println!("cargo:rustc-link-lib=GL");
}
