# Host regression checks

These tests exercise the CPU rasterizer and repaint callback without building
PhotoCraft or linking AROS libraries. Run from `photocraft-aros`:

```sh
cargo test --manifest-path tests/host/Cargo.toml --locked
python3 tests/test_repaint.py
```

Use a Rust toolchain supporting egui 0.36.2 and the checked-in lockfile
(verified with rustc 1.100.0-nightly, 0dfb098f3).

The raster tests compile the actual src/raster.rs. A test-only thread-local
counter checks that equal UVs sample once, while varying UVs still sample per
pixel. Output checks cover clipping, both winding orders, nearest/linear
sampling, flat/gradient vertex colours and transparent/premultiplied colours.
They do not claim exhaustive floating-point equivalence for all geometry.

The repaint script extracts the actual callback from src/main.rs and compiles
it with a counted replacement for the Intuition wake operation. It checks
UI-thread deadlines without self-waking, worker wake-up, and preservation of
an earlier deadline. It does not exercise timer.device or guest scheduling.

Before the fixes, the sample-count test fails with 21 instead of 1 and the
UI-thread wake test fails with 1 instead of 0. Both pass after the fixes.
These host checks are not a mainline AROS build or runtime acceptance.
