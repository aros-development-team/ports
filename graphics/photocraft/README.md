# PhotoCraft for AROS

[PhotoCraft](https://github.com/storytold/photocraft) is an open-source image
editor written in Rust (layers, masks, adjustment layers, layer styles, type,
vectors, brushes, PSD files). This directory builds it for AROS with the Rust
crosstool (`configure --enable-rust`, see `tools/crosstools/rust`) and installs

* `PhotoCraft`, the editor, in `Extras:MultiMedia/Gfx/PhotoCraft`, and
* `photocraft-cli`, the headless command line (convert, info, run engine
  commands, batch, droplets, a JSON-lines automation server on stdin/stdout).

`make ports-graphics-photocraft` builds both; `ports-graphics-photocraft-quick`
rebuilds without re-fetching.

## How it is built

* The upstream source snapshot (`PHOTOCRAFT_REV` in the mmakefile, a commit
  on main) is fetched from GitHub with the AROS fetch macro and patched with
  `photocraft-<version>-aros.diff`.
* The crates.io crates that need AROS changes come from the shared crate
  stores: `libc` (the AROS module), `getrandom` (a `getentropy()` backend)
  and `parking_lot_core` (a realtime-clock condition variable) from
  `tools/crosstools/rust/crates`, and `fontique` (reads font files instead of
  memory-mapping them, AROS has no `mmap`) from
  `contrib/development/rust/crates`. Their generated cargo configurations
  replace the crates.io versions.
* `photocraft-aros` (this directory) is linked into the upstream workspace as
  `apps/photocraft-aros`.
* `cargo fetch` downloads the remaining crates once into the shared cargo
  home (`$(PORTSSOURCEDIR)/cargo-home`); this is the one step that needs
  network access. Builds then run with `--offline`.
* `cargo build --release` for the target produces both programs, which are
  copied to the contrib directory with the icon from `icons/`.

`ports-graphics-photocraft-create-patch` regenerates the upstream diff from the
patched tree.

## The AROS shell

PhotoCraft's desktop app runs its egui interface on eframe (winit windows, a
wgpu renderer), none of which exists on AROS. `photocraft-aros` drives the
same interface crate (`photocraft-ui-egui`) directly:

* `c/aros_shell.c` opens an Intuition window on the default public screen,
  turns its IDCMP messages into plain event records (mouse, wheel, raw keys
  mapped through keymap.library), blits the frame with cybergraphics,
  shows asl.library file requesters, reads and writes the FTXT clipboard,
  and sleeps on a timer.device request until egui's next repaint is due.
* `src/raster.rs` rasterizes egui's tessellated output (textured, vertex
  coloured triangles with premultiplied alpha) in software.
* `src/services.rs` provides the platform services the interface asks for
  (file dialogs, atomic saves, preferences, crash recovery), following the
  desktop app's `services.rs`.
* While background work runs (file opens and saves, the font scan) the event
  loop lowers its task priority to -1 so the worker threads, at priority 0,
  get the CPU: a task that keeps rendering is not time-sliced against them
  on every AROS flavour, and the loop only needs to draw when they wait.

Settings, presets, recovery files and logs live in `PROGDIR:PhotoCraftData`
(or `PHOTOCRAFT_CONFIG_DIR`). Fonts for the Type tool come from
`FONTS:TrueType` (and `PHOTOCRAFT_FONT_DIR`).

`PhotoCraft --screenshot <file.png> [--screenshot-frames <n>]` writes the
window's contents after `n` frames (20 by default) and quits, for looking at
the interface on a machine whose display cannot be watched.

## What the upstream patch changes

* `crates/ui-egui`: eframe becomes an optional feature (on by default; the
  desktop and web apps are unchanged). `impl eframe::App` forwards to
  `PhotocraftApp::shell_logic`, `shell_raw_input_hook` and `shell_ui`, which a
  shell can call itself; the GPU canvas uses `egui-wgpu` directly (wgpu builds
  with no backend behind it, so the CPU canvas path is what runs); the file
  dialog's parent window is a `WindowHandle` alias.
* `crates/automation`: the MCP server (rmcp, tokio) and the cap-std
  filesystem capabilities become the features `mcp` and `sandbox` (on by
  default). Without `sandbox` the same path policy is applied before plain
  `std::fs` access beneath the configured roots. `photocraft-cli` gets a
  matching `mcp` feature. AROS builds with `--no-default-features`: there are
  no sockets in the AROS C library, and cap-std's rustix does not build for
  the target.
* `crates/text`: `FONTS:TrueType` among the system font directories.
* `crates/format` and `crates/ui-egui`: AmigaDOS paths. A `VOLUME:` path is
  absolute already, a bare file name is written without creating "." first,
  and a file's temporary sibling is named after the last `/` or `:`.

## Limitations

* The canvas is composited on the CPU and the interface is rasterized in
  software; there is no GPU path.
* The TCP control channel, the MCP server and HEIF input are not built.
* Images are not exchanged with the clipboard (text is); files dropped on the
  window are not received.
