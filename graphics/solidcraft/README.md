# SolidCraft for AROS

[SolidCraft](https://github.com/tangency-labs/solidcraft) is an open-source
parametric 3D CAD application written in Rust: sketches with a constraint
solver, a timeline of features, a B-rep kernel, STEP, IGES, 3MF, STL, OBJ and
glTF files. This directory builds it for AROS with the Rust crosstool
(`configure --enable-rust`, see `tools/crosstools/rust`) and installs

* `SolidCraft`, the application, in `Extras:MultiMedia/Gfx/SolidCraft`, and
* `solidcraft-cli`, the headless command line (run command scripts, measure,
  export, render snapshots, the MCP server on stdin/stdout).

`make ports-graphics-solidcraft` builds both; `ports-graphics-solidcraft-quick`
rebuilds without re-fetching.

## How it is built

* The upstream source snapshot (`SOLIDCRAFT_REV` in the mmakefile, a commit
  on main) is fetched from GitHub with the AROS fetch macro and patched with
  `solidcraft-<version>-aros.diff`.
* The crates.io crates that need AROS changes come from the shared crate
  stores: `libc` (the AROS module), `getrandom` (a `getentropy()` backend)
  and `parking_lot_core` (a realtime-clock condition variable) from
  `tools/crosstools/rust/crates`; `raw-window-handle` (the `Aros` window
  handle), `libloading`, and wgpu 30 (`wgpu`, `wgpu-core`, `wgpu-hal`,
  `naga`, `naga-types`) from `contrib/development/rust/crates`. The wgpu
  crates carry the same changes as the wgpu 27 Veloren uses: the OpenGL
  backend on gl.library's GLA contexts, no Vulkan, no RenderDoc or Wayland
  loader (both need `dlopen()`), and the GL 3.3 extensions Mesa's softpipe
  has (storage buffers, std430, compute shaders) accepted by naga's GLSL
  backend. naga also learned to read a depth texture with `textureLoad`
  (the viewport's ambient-occlusion depth map): a depth image that is only
  loaded, never compared, is declared as a plain sampler and fetched with
  `texelFetch`. Their generated cargo configurations replace the crates.io
  versions.
* `solidcraft-aros` (this directory) is linked into the upstream workspace as
  `apps/solidcraft-aros`.
* `cargo fetch` downloads the remaining crates once into the shared cargo
  home (`$(PORTSSOURCEDIR)/cargo-home`); this is the one step that needs
  network access. Builds then run with `--offline`.
* `cargo build --release` for the target produces both programs, which are
  copied to the contrib directory with the icon from `icons/`.

`ports-graphics-solidcraft-create-patch` regenerates the upstream diff from
the patched tree.

## The AROS shell

SolidCraft's desktop app runs its egui interface on eframe (winit windows,
wgpu on Vulkan, Metal or D3D12), none of which exists on AROS. The 3D viewport
is drawn by wgpu render pipelines inside the egui frame (there is no CPU path
for it), so the shell keeps wgpu and gives it OpenGL: `solidcraft-aros`
drives the interface crate (`solidcraft-ui-egui`) directly and draws with
egui-wgpu on wgpu's OpenGL backend, which the crate store's `wgpu-hal` runs on
gl.library (Mesa's softpipe on most systems).

* `src/window.rs` opens an Intuition window through `aros-gui-sys` (the glue
  crate of `tools/crosstools/rust/crates`), reads its IDCMP messages, sleeps
  on a timer until egui's next repaint is due with a signal workers can wake
  it with, shows asl.library file requesters, and reads and writes the FTXT
  clipboard. `c/aros_extra.c` adds the busy pointer, DisplayBeep, OpenURL
  and the task priority.
* `src/input.rs` turns the messages into egui events: pointer, buttons,
  the NewMouse wheel, raw keys mapped through keymap.library, typed text,
  and Ctrl+C/X/V as egui's clipboard events.
* `src/painter.rs` makes the wgpu surface on the window (the `Aros` raw
  window handle), egui-wgpu's render state with the depth and stencil buffer
  the viewport's pipelines expect (a 32-bit float depth buffer with a stencil
  where the adapter has it, else 24 bits with a stencil; reversed depth
  follows), and renders a frame: textures, buffers, one render pass with the
  viewport's paint callbacks inside, present. MSAA is off: on OpenGL the
  resolve path is unreliable (the desktop app turns it off there too).
* `src/services.rs` provides the platform services the interface asks for:
  the Open, Save As, file and program pickers on ASL requesters (answered
  through the interface's picker futures), and the graphics settings file.
  There is no HTTPS (the update check and the API agents of the agent chat
  are off), no keychain, and no file manager to show a folder in.
* `src/main.rs` does what the desktop app does around the interface: the
  crash reports, the preferences, the sample design, files on the command
  line, the start page, autosave and recovery, the close gadget asking about
  unsaved designs, and the frame loop. The interface runs on a thread with a
  64 MB stack (the kernel's operations go deep); the main task waits for it.

Settings, recovery files and crash reports live in `PROGDIR:SolidCraftData`
(or `SOLIDCRAFT_DATA_DIR`). The interface thread brings its own 64 MB stack;
`SOLIDCRAFT_AROS_UI_THREAD=0` runs it on the program's task instead, whose
stack the icon (10 MB) or the `Stack` command sets.

`SolidCraft --screenshot <file.png> [--screenshot-frames <n>]` writes the
window's contents after `n` frames (20 by default) and quits, for looking at
the interface on a machine whose display cannot be watched; `--sample` opens
the sample design, `--safe-mode` starts with default settings and bodies
meshed on the CPU.

## What the upstream patch changes

* `crates/ui-egui`: `arboard` (pictures and file lists from the clipboard,
  for the agent chat) is not built on AROS, where the clipboard carries text
  only; the chat's paste answers accordingly.
* `crates/io`: `cadmpeg` (FreeCAD and Rhino import) is not built on AROS:
  it compiles the zstd C library and brings `tempfile`/`rustix`, which do
  not build for the target. Those files report that they are not supported.
* `crates/engine`: the per-user data folder (recovery, crashes) is
  `PROGDIR:SolidCraftData` on AROS; a component link's path is left alone
  when it names a device or volume (`Work:Designs/a.solidcraft` is absolute
  already, and std would put the current directory in front of it).
* `crates/ui-egui` (agent chat attachments): the same for attached files;
  `fonts.rs` looks in `FONTS:TrueType` for the CJK and Chinese fallbacks.
* `crates/gpu`: headless pictures (`solidcraft-cli snapshot`, thumbnails,
  the agent's view) are drawn without multisampling on OpenGL, where the
  resolve path gives a blank picture on Mesa's software rasterizers (as
  egui-wgpu already does for the window).

## Status

Tested on hosted AROS (linux-x86_64, Mesa 26 softpipe, GL 3.3 core): the
window opens, the interface and the viewport draw, the sample design loads
and `--screenshot` writes it; `solidcraft-cli run` exports STEP and
`snapshot` renders. Mesa's C++ must be built with `-flifetime-dse=1` (as
mesa.cfg asks); without it the GLSL compiler faults in `linear_alloc_child`
on the first shader.

## Limitations

* Rendering goes through Mesa's software rasterizer; large models are slow
  to orbit. Display tessellation runs on the GPU only where the OpenGL
  context has compute shaders (GL 4.3); elsewhere bodies are meshed on the
  CPU, as in safe mode.
* No update check, no agent chat API agents, no 3D mouse, no control server
  (`--control`: the AROS C library has no sockets), no FreeCAD or Rhino
  import, no pictures from the clipboard, no files dropped on the window.
