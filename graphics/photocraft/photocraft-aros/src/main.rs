//! PhotoCraft on AROS.
//!
//! Usage: `PhotoCraft [--screenshot <file.png> [--screenshot-frames <n>]] [files…]`
//!
//! The egui interface (`photocraft-ui-egui`) is driven directly: an Intuition window
//! (`c/aros_shell.c`, `ffi`) delivers input, the tessellated frame is rasterized in software
//! (`raster`) and blitted into the window. The engine, documents and file formats are the same
//! as on the other platforms.
//!
//! `--screenshot <file>` writes the window's pixels after `--screenshot-frames` frames (20 by
//! default) and quits: a way to look at the interface where no display can be watched.
#![deny(clippy::unwrap_used, clippy::expect_used, clippy::panic, clippy::unimplemented, clippy::todo, clippy::unreachable)]

mod ffi;
mod input;
mod raster;
mod services;

use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use egui::{OutputCommand, Rect, ViewportCommand, ViewportEvent, ViewportId, pos2, vec2};
use photocraft_engine::Session;
use photocraft_ui_egui::PhotocraftApp;

/// Frames between polls when nothing asks for a repaint: background jobs and recovery
/// autosaves still get their turn.
const IDLE_POLL: Duration = Duration::from_millis(500);

struct Logger;

impl log::Log for Logger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= log::max_level()
    }

    fn log(&self, record: &log::Record) {
        if self.enabled(record.metadata()) {
            eprintln!("photocraft: {} {}: {}", record.level(), record.target(), record.args());
        }
    }

    fn flush(&self) {}
}

static LOGGER: Logger = Logger;

fn install_logger() {
    let level = match std::env::var("RUST_LOG").ok().as_deref().map(str::trim) {
        Some("trace") => log::LevelFilter::Trace,
        Some("debug") => log::LevelFilter::Debug,
        Some("info") => log::LevelFilter::Info,
        Some("error") => log::LevelFilter::Error,
        Some("off") => log::LevelFilter::Off,
        _ => log::LevelFilter::Warn,
    };
    if log::set_logger(&LOGGER).is_ok() {
        log::set_max_level(level);
    }
}

struct Options {
    files: Vec<String>,
    screenshot: Option<String>,
    screenshot_frames: u64,
}

fn parse_args() -> Result<Option<Options>, String> {
    let mut o = Options { files: Vec::new(), screenshot: None, screenshot_frames: 20 };
    let mut args = std::env::args_os().skip(1);
    while let Some(a) = args.next() {
        match a.to_str() {
            Some("--help") | Some("-h") => {
                println!("PhotoCraft [--screenshot <file.png> [--screenshot-frames <n>]] [files...]");
                return Ok(None);
            }
            Some("--version") => {
                println!("photocraft {}", photocraft_engine::build_info::long_version());
                return Ok(None);
            }
            Some("--screenshot") => o.screenshot = args.next().map(|p| p.to_string_lossy().into_owned()),
            Some("--screenshot-frames") => {
                let v = args.next().map(|p| p.to_string_lossy().into_owned()).unwrap_or_default();
                o.screenshot_frames = v.parse().map_err(|_| format!("--screenshot-frames: `{v}` is not a number"))?;
            }
            Some(s) => o.files.push(s.to_owned()),
            None => return Err(format!("argument is not valid text: {}", a.to_string_lossy())),
        }
    }
    Ok(Some(o))
}

fn main() {
    install_logger();
    let options = match parse_args() {
        Ok(Some(o)) => o,
        Ok(None) => return,
        Err(e) => {
            eprintln!("photocraft: {e}");
            std::process::exit(2);
        }
    };
    if let Some(dir) = services::config_dir()
        && let Err(e) = std::fs::create_dir_all(&dir)
    {
        log::warn!("cannot create the settings directory {}: {e}", dir.display());
    }
    // The font scan runs off the UI thread (see the desktop app).
    photocraft_ui_egui::type_tool::warm_fonts();

    let (sw, sh) = ffi::screen_size();
    let (w, h) = (1440.min(sw - 16).max(640), 900.min(sh - 48).max(400));
    let Some(window) = ffi::Window::open(w, h, "PhotoCraft") else {
        eprintln!("photocraft: cannot open a window");
        std::process::exit(1);
    };
    let window = Rc::new(window);
    let shared_window: services::SharedWindow = Rc::new(RefCell::new(Some(window.clone())));
    let code = run(&window, shared_window.clone(), options);
    // The services hold the window too; let go before closing it.
    *shared_window.borrow_mut() = None;
    drop(window);
    std::process::exit(code);
}

/// Repaint requests from egui (any thread): the earliest deadline, and the window's wake-up.
struct Repaint {
    waker: ffi::Waker,
    due: Mutex<Option<Instant>>,
}

fn run(window: &Rc<ffi::Window>, shared_window: services::SharedWindow, options: Options) -> i32 {
    let ctx = egui::Context::default();
    let repaint = Arc::new(Repaint { waker: window.waker(), due: Mutex::new(Some(Instant::now())) });
    {
        let repaint = repaint.clone();
        let ui_thread = std::thread::current().id();
        ctx.set_request_repaint_callback(move |info| {
            let due = Instant::now() + info.delay;
            let mut slot = repaint.due.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            *slot = Some(slot.map_or(due, |d| d.min(due)));
            drop(slot);
            // UI-thread requests already update the deadline read before the next wait.
            // Signalling here would make that wait return immediately, even for a
            // delayed caret blink. Background requests still need to interrupt it.
            if std::thread::current().id() != ui_thread {
                repaint.waker.wake();
            }
        });
    }
    let quit = Rc::new(Cell::new(false));
    let mut app = PhotocraftApp::new(Session::new(), services::aros(shared_window, quit.clone()));
    app.integrated_titlebar = false;
    app.custom_titlebar = false;
    // Long commands and file opens run as background jobs with progress and Cancel.
    app.background_jobs = std::env::var_os("PHOTOCRAFT_INLINE_JOBS").is_none();
    app.services.preset_store = services::presets_dir().map(photocraft_engine::preset_store::open_dir_async);
    if !options.files.is_empty() {
        app.open_paths(&options.files);
    }

    let ppp = 1.0f32;
    let (mut width, mut height) = window.inner_size();
    let mut renderer = raster::Renderer::new(width as usize, height as usize);
    let mut input = input::Input::default();
    let start = Instant::now();
    let mut frame: u64 = 0;
    let mut focused = true;
    let mut busy = false;
    let mut lowered = false;
    let mut title = String::from("PhotoCraft");
    let (sw, sh) = ffi::screen_size();

    loop {
        // Sleep until something happens or the next repaint is due.
        let due = repaint.due.lock().unwrap_or_else(std::sync::PoisonError::into_inner).take();
        let timeout = match due {
            Some(t) => t.saturating_duration_since(Instant::now()).min(IDLE_POLL),
            None => IDLE_POLL,
        };
        window.wait(timeout.as_millis() as i32);
        while let Some(ev) = window.poll() {
            input.push(&ev, ppp);
        }
        if let Some((nw, nh)) = input.resized.take() {
            width = nw.max(1);
            height = nh.max(1);
            renderer.resize(width as usize, height as usize);
        }
        if let Some(f) = input.focus.take() {
            focused = f;
        }
        let close_requested = std::mem::take(&mut input.close_requested);

        // This frame's input.
        let screen_rect = Rect::from_min_size(pos2(0.0, 0.0), vec2(width as f32 / ppp, height as f32 / ppp));
        let mut raw = egui::RawInput {
            screen_rect: Some(screen_rect),
            max_texture_side: Some(16384),
            time: Some(start.elapsed().as_secs_f64()),
            predicted_dt: 1.0 / 60.0,
            events: input.take_events(),
            focused,
            ..Default::default()
        };
        if std::mem::take(&mut input.paste_pending) {
            raw.events.push(egui::Event::Paste(ffi::clipboard_read_text().unwrap_or_default()));
        }
        {
            let info = raw.viewports.entry(ViewportId::ROOT).or_default();
            info.native_pixels_per_point = Some(ppp);
            info.monitor_size = Some(vec2(sw as f32 / ppp, sh as f32 / ppp));
            info.inner_rect = Some(screen_rect);
            info.outer_rect = Some(screen_rect);
            info.focused = Some(focused);
            info.title = Some(title.clone());
            if close_requested {
                info.events.push(ViewportEvent::Close);
            }
        }
        app.shell_raw_input_hook(&ctx, &mut raw);

        // The frame.
        let out = ctx.run_ui(raw, |ui| {
            app.shell_logic(ui.ctx(), None);
            app.shell_ui(ui, None);
        });
        frame += 1;

        // What the frame asked of the platform.
        for cmd in &out.platform_output.commands {
            match cmd {
                OutputCommand::CopyText(text) => {
                    if !ffi::clipboard_write_text(text) {
                        log::warn!("could not write the clipboard");
                    }
                }
                OutputCommand::CopyImage(_) => log::info!("copying images to the clipboard is not supported on AROS"),
                OutputCommand::OpenUrl(url) => {
                    if !ffi::open_url(&url.url) {
                        log::warn!("could not open {}", url.url);
                    }
                }
            }
        }
        // Background work in progress (jobs, the font scan): step aside for it, see
        // `pc_window_set_priority`. Equal priority again once everything is done.
        let background = app.session.has_jobs() || photocraft_text::try_shared().is_none();
        if background != lowered {
            lowered = background;
            window.set_priority(if lowered { -1 } else { 0 });
        }
        let wants_busy = out.platform_output.cursor_icon == egui::CursorIcon::Wait;
        if wants_busy != busy {
            busy = wants_busy;
            window.set_busy(busy);
        }
        let mut closing = close_requested;
        if let Some(vo) = out.viewport_output.get(&ViewportId::ROOT) {
            for cmd in &vo.commands {
                match cmd {
                    ViewportCommand::Close => closing = true,
                    ViewportCommand::CancelClose => closing = false,
                    ViewportCommand::Title(t) => {
                        if *t != title {
                            title = t.clone();
                            window.set_title(&title);
                        }
                    }
                    ViewportCommand::RequestPaste => input.paste_pending = true,
                    _ => {}
                }
            }
            // The next repaint: at once, after a delay, or when input arrives.
            let delay = vo.repaint_delay;
            if delay < Duration::from_secs(3600) {
                let due = Instant::now() + delay;
                let mut slot = repaint.due.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
                *slot = Some(slot.map_or(due, |d| d.min(due)));
            }
        }

        // Paint and show.
        renderer.update_textures(&out.textures_delta);
        let primitives = ctx.tessellate(out.shapes, out.pixels_per_point);
        renderer.clear([0x1e, 0x1e, 0x1e]);
        renderer.paint(&primitives, out.pixels_per_point);
        renderer.free_textures(&out.textures_delta);
        window.blit(&renderer.frame, renderer.width, renderer.height, renderer.stride());

        if let Some(path) = options.screenshot.as_deref()
            && frame >= options.screenshot_frames
        {
            match write_png(path, &renderer) {
                Ok(()) => eprintln!("photocraft: wrote {path}"),
                Err(e) => eprintln!("photocraft: screenshot {path}: {e}"),
            }
            return 0;
        }
        if closing || quit.get() {
            return 0;
        }
    }
}

fn write_png(path: &str, r: &raster::Renderer) -> Result<(), String> {
    let img = photocraft_codecs::Image::from_u8(r.width as u32, r.height as u32, photocraft_codecs::ChannelLayout::Rgba, r.frame.clone())
        .map_err(|e| e.to_string())?;
    let bytes = photocraft_codecs::encode(&img, photocraft_codecs::Format::Png, &photocraft_codecs::EncodeOptions::default()).map_err(|e| e.to_string())?;
    std::fs::write(path, bytes).map_err(|e| e.to_string())
}
