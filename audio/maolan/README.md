# Maolan for AROS

[Maolan](https://github.com/maolan/maolan) is an open-source digital audio
workstation written in Rust (audio and MIDI tracks, clips, routing,
automation, mixing, export). This directory builds it for AROS with the Rust
crosstool (`configure --enable-rust`, see `tools/crosstools/rust`) and
installs `Maolan` in `Extras:MultiMedia/Audio/Maolan`.

`make ports-audio-maolan` builds it; `ports-audio-maolan-quick`
rebuilds without re-fetching.

To try it: start `Maolan`, press "Open Audio" on the setup screen (the AHI
mode AHI considers best is preselected), then play. `Maolan --session
<folder>` opens a session folder once the audio device is up;
`Examples/OdeToJoy` next to the program is one (a track with a synthesized
clip, routed to the hardware outputs).

## How it is built

* The upstream source snapshot (`MAOLAN_REV` in the mmakefile, a commit on
  main) is fetched from GitHub with the AROS fetch macro and patched with
  `maolan-<version>-aros.diff`.
* The crates.io crates that need AROS changes come from the shared crate
  stores (`tools/crosstools/rust/crates` and `contrib/development/rust/crates`,
  see the README there): `libc`, `getrandom`, `parking_lot_core`; `mio`,
  `socket2`, `tokio`, `errno` (tokio runs without its I/O driver on AROS),
  `libloading`, `iced` with the `iced_aros` shell in place of `iced_winit`,
  `iced_tiny_skia` without softbuffer, and `rfd-aros` in place of `rfd`.
* Two crates are patched here: `maolan-engine` gets the AHI backend
  (`hw/ahi.rs`, `hw/ahi_glue.c`, `workers/ahi_worker.rs`) and
  `maolan-plugin-host` compiles without the realtime and shared-library
  setup it uses elsewhere. The stand-ins in `crates/`
  (`maolan-generate-stub`, `maolan-pitch-stub`) keep the generation and
  neural pitch dialogs compiling without the Burn/wgpu machine-learning
  stack (they report that the feature is unavailable).
* `cargo fetch` downloads the remaining crates once into the shared cargo
  home (`$(PORTSSOURCEDIR)/cargo-home`); this is the one step that needs
  network access. Builds then run with `--offline`.
* `cargo build --release --bin maolan` for the target produces the program,
  which is copied to the contrib directory with the icon from `icons/`.

`ports-audio-maolan-create-patch` regenerates the upstream diff from
the patched tree (the crate diffs are made by hand against the unpacked
`.crate` archives, with `diff -ruN` from inside the crate directory).

## The AROS shell (`contrib/development/rust/crates/aros/iced_aros`)

Maolan's interface is an iced 0.14 program; iced runs it on winit windows
with a wgpu or tiny-skia renderer behind softbuffer, none of which exists on
AROS. `iced_aros` replaces `iced_winit` for one window:

* `c/aros_shell.c` opens an Intuition window on the default public screen,
  turns its IDCMP messages into plain event records (mouse, wheel, raw keys
  mapped through keymap.library), blits the frame with cybergraphics, reads
  and writes the FTXT clipboard, and sleeps on a timer.device request until
  the next frame is due or a task wakes it.
* `src/lib.rs` runs iced's program loop: the user interface is built from
  the program's view, updated with the window's events, drawn with the
  tiny-skia renderer through its headless path, and blitted; tasks and
  subscriptions run on the program's tokio executor and deliver their
  actions through a proxy that wakes the window's task.
* `src/conversion.rs` maps the window's events to iced events (keys by raw
  key code for the special keys, by the keymap's character otherwise).

The program loop runs on a thread with a 64 MB stack (an iced view of this
size needs more than an AROS process starts with). Fonts: the bundled Fira
Sans, Maolan's icon fonts, and the files in `FONTS:TrueType`; the generic
families (sans-serif and so on) are pointed at Fira Sans when no system font
provides them.

For testing without a display to watch: `ICED_AROS_SCREENSHOT=<file.png>`
(and `ICED_AROS_SCREENSHOT_AFTER=<ms>`, 3000 by default) makes the shell
write the window's contents to a PNG and quit, and
`ICED_AROS_AUTOCLICK=<ms>:<x>,<y>[+...]` clicks at the given points.

## Audio

The engine's hardware backend on AROS is AHI (`maolan-engine/src/hw/ahi.rs`
with `ahi_glue.c`): the low-level AHI API with a ring of dynamic sample
sounds for playback (`N periods` slots of `Period frames` each, played back to
back; the sound hook queues the next slot and frees the finished one) and the
record hook for capture, at the mixing frequency the chosen mode supports.
AHI drivers tie their start/stop handshake to the task that allocated the
audio, while the engine opens the driver on one thread and runs its cycles on
a pool of others, so the glue keeps a control task of its own for every AHI
control call; the cycle threads only fill the ring and wait (with a signal
bit each task registers for itself, and a timer bound so a missing sound
hook costs an xrun, not a hang). Devices are listed from
`AHI_NextAudioID()` as `AHI:0x<audio id>`; the default is
`AHI_BestAudioID()` for a stereo 16-bit mode. One mode serves playback and
recording, so the setup screen has no separate input device on AROS;
recording is enabled when the mode supports it. There is no hardware MIDI
backend (MIDI clips play through the engine's instruments only).

The engine's cycle thread asks for priority 18 through `pthread_setschedparam`,
which AROS pthreads map straight onto the task priority.

## What the upstream patch changes

* `Cargo.toml`: the Hugging Face hub download, nix, x11-dl and the terminal
  tools' crates are not dependencies on AROS.
* `src/state`, `src/gui`, `src/consts.rs`, `src/message.rs`, `src/hw.rs`:
  `AudioBackendOption::Ahi` next to the FreeBSD/OpenBSD backends, AHI device
  discovery (`state/platform_aros.rs`), and `SUPPORTS_PLUGIN_GRAPH` false.
* `src/gui/update/dispatch/generate.rs`: no `kill()` of generation
  processes.
* `src/main.rs`, `src/gui`: the `--session <folder>` option (opened once the
  audio device is up), and the MixOSC output socket is not tried on AROS.

## Limitations

* Plug-ins (LV2, VST3, CLAP) are not hosted: plug-in hosting runs plug-ins in
  a separate process over shared memory and sockets, and AROS has neither
  `dlopen` nor `shm_open`/`socketpair`. The plug-in lists stay empty.
* Audio and MIDI generation (ACE-Step, text-to-MIDI) and the neural pitch
  detector and vocoder are not available: they need the Burn/wgpu
  machine-learning stack. The dialogs report this when used; the classic
  pitch detector works.
* JACK is not available; AHI is the only backend.
* The interface is rendered in software (tiny-skia); there is no GPU path.
* Files dropped on the window are not received, and only text is exchanged
  with the clipboard.
