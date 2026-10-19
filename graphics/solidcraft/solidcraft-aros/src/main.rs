//! SolidCraft on AROS.
//!
//! Usage: `SolidCraft [--sample] [--safe-mode] [--screenshot <file.png> [--screenshot-frames <n>]] [design.solidcraft | part.step]`
//!
//! The egui interface (`solidcraft-ui-egui`) is driven directly: an Intuition window (`window`,
//! through aros-gui-sys) delivers input, and egui-wgpu draws the frame, viewport included, with
//! wgpu's OpenGL backend on gl.library (`painter`). The engine, documents and file formats are
//! the same as on the other platforms; what the desktop app (apps/solidcraft) does around the
//! interface is done here without eframe.
//!
//! `--screenshot <file>` writes the window's pixels after `--screenshot-frames` frames (20 by
//! default) and quits: a way to look at the interface where no display can be watched.
#![deny(clippy::unwrap_used, clippy::expect_used, clippy::panic, clippy::unimplemented, clippy::todo, clippy::unreachable)]
#![cfg_attr(not(target_os = "aros"), allow(dead_code, unused_imports))]

#[cfg(target_os = "aros")]
mod input;
#[cfg(target_os = "aros")]
mod painter;
#[cfg(target_os = "aros")]
mod services;
#[cfg(target_os = "aros")]
mod window;

#[cfg(not(target_os = "aros"))]
fn main() {
    eprintln!("SolidCraft's AROS shell only runs on AROS");
    std::process::exit(1);
}

#[cfg(target_os = "aros")]
mod app {
    use std::rc::Rc;
    use std::sync::{Arc, Mutex};
    use std::time::{Duration, Instant};

    use egui::{OutputCommand, Rect, ViewportCommand, ViewportEvent, ViewportId, pos2, vec2};
    use solidcraft_engine::Session;
    use solidcraft_ui_egui::SolveApp;

    use crate::painter::{self, Painter};
    use crate::services;
    use crate::window::{self, Window};

    /// Frames between polls when nothing asks for a repaint: background work (file loads,
    /// renders, autosaves) still gets its turn.
    const IDLE_POLL: Duration = Duration::from_millis(500);
    /// How often the preferences are written while the app runs (eframe's autosave interval).
    const PREFS_INTERVAL: Duration = Duration::from_secs(30);
    /// The interface's thread runs with this stack: the kernel's operations and the dialogs go
    /// deep, and a shell's default stack is small.
    const STACK_SIZE: usize = 64 << 20;

    struct Logger;

    impl log::Log for Logger {
        fn enabled(&self, metadata: &log::Metadata) -> bool {
            metadata.level() <= log::max_level()
        }

        fn log(&self, record: &log::Record) {
            if self.enabled(record.metadata()) {
                eprintln!("solidcraft: {} {}: {}", record.level(), record.target(), record.args());
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
        sample: bool,
        safe_mode: bool,
        screenshot: Option<String>,
        screenshot_frames: u64,
    }

    fn parse_args() -> Result<Option<Options>, String> {
        let mut o = Options { files: Vec::new(), sample: false, safe_mode: false, screenshot: None, screenshot_frames: 20 };
        let mut args = std::env::args_os().skip(1);
        while let Some(a) = args.next() {
            match a.to_str() {
                Some("--help") | Some("-h") => {
                    println!("SolidCraft [--sample] [--safe-mode] [--screenshot <file.png> [--screenshot-frames <n>]] [design.solidcraft | part.step]");
                    return Ok(None);
                }
                Some("--version") | Some("-V") => {
                    println!("solidcraft {}", solidcraft_ui_egui::help::VERSION);
                    return Ok(None);
                }
                Some("--sample") => o.sample = true,
                Some("--safe-mode") => o.safe_mode = true,
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

    pub fn main() {
        install_logger();
        let options = match parse_args() {
            Ok(Some(o)) => o,
            Ok(None) => return,
            Err(e) => {
                eprintln!("solidcraft: {e}");
                std::process::exit(2);
            }
        };
        // The interface runs on a thread with a large stack; the main task waits for it and
        // ends the process (exit() must be called on the main task). SOLIDCRAFT_AROS_UI_THREAD=0
        // runs it on the main task instead (whose stack the icon or the Stack command sets).
        if solidcraft_engine::geom::env::var("AROS_UI_THREAD").is_some_and(|v| v.trim() == "0") {
            let code = run(options);
            if code != 0 {
                std::process::exit(code);
            }
            return;
        }
        let worker = std::thread::Builder::new().name("solidcraft-ui".into()).stack_size(STACK_SIZE).spawn(move || run(options));
        let code = match worker.map(std::thread::JoinHandle::join) {
            Ok(Ok(code)) => code,
            Ok(Err(_)) => {
                eprintln!("solidcraft: the interface thread panicked");
                1
            }
            Err(e) => {
                eprintln!("solidcraft: cannot start the interface thread: {e}");
                1
            }
        };
        if code != 0 {
            std::process::exit(code);
        }
    }

    /// Repaint requests from egui (any thread): the earliest deadline, and the window's wake-up.
    struct Repaint {
        waker: window::Waker,
        due: Mutex<Option<Instant>>,
    }

    impl Repaint {
        fn ask(&self, at: Instant) {
            let mut slot = self.due.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            *slot = Some(slot.map_or(at, |d| d.min(at)));
        }
    }

    fn run(options: Options) -> i32 {
        let config_dir = services::config_dir();
        if let Err(e) = std::fs::create_dir_all(&config_dir) {
            log::warn!("cannot create the settings directory {}: {e}", config_dir.display());
        }
        let safe_mode = options.safe_mode || solidcraft_engine::geom::env::var("SAFE_MODE").is_some_and(|v| !v.trim().is_empty() && v.trim() != "0");
        // Crashes leave a report in the crash folder (solidcraft_engine::crash, docs/crash-reports.md).
        let crashed = solidcraft_engine::crash::default_dir()
            .map(|dir| {
                let build = solidcraft_engine::crash::Build {
                    version: solidcraft_ui_egui::help::VERSION,
                    commit: solidcraft_ui_egui::help::COMMIT,
                    built: solidcraft_ui_egui::help::BUILD_DATE,
                };
                solidcraft_engine::crash::install(dir, build)
            })
            .unwrap_or_default();
        solidcraft_engine::crash::phase("window");

        let (sw, sh) = window::screen_size();
        let (w, h) = (1600.min(sw - 16).max(640), 1000.min(sh - 48).max(400));
        let Some(window) = Window::open(w, h, "SolidCraft") else {
            eprintln!("solidcraft: cannot open a window");
            return 1;
        };
        solidcraft_engine::crash::phase("graphics device");
        let mut painter = match Painter::new(&window) {
            Ok(p) => p,
            Err(e) => {
                eprintln!("solidcraft: the window could not be drawn: {e}");
                return 1;
            }
        };
        let window = Rc::new(window);
        let code = {
            let window_ref = window.clone();
            app_loop(&window_ref, &mut painter, options, safe_mode, crashed)
        };
        // The painter's surface was made on the window: it goes first.
        drop(painter);
        drop(window);
        code
    }

    fn app_loop(window: &Rc<Window>, painter: &mut Painter, options: Options, safe_mode: bool, crashed: Vec<solidcraft_engine::crash::Summary>) -> i32 {
        let ctx = egui::Context::default();
        solidcraft_engine::crash::phase("app setup");
        solidcraft_ui_egui::fonts::setup_fonts(&ctx);
        let repaint = Arc::new(Repaint { waker: window.waker(), due: Mutex::new(Some(Instant::now())) });
        {
            let repaint = repaint.clone();
            ctx.set_request_repaint_callback(move |info| {
                repaint.ask(Instant::now() + info.delay);
                repaint.waker.wake();
            });
        }
        let mut app = SolveApp::new(Session::default(), services::aros(window.clone()));
        app.apply_off_thread = Some(solidcraft_ui_egui::dialogs::APPLY_AT_ONCE);
        app.custom_titlebar = false;
        app.integrated_titlebar = false;
        app.cpu_meshing = safe_mode;
        let prefs_file = services::prefs_file();
        if !safe_mode && let Ok(p) = std::fs::read_to_string(&prefs_file) {
            app.load_prefs(&p);
        }
        let mut notes: Vec<String> = Vec::new();
        if safe_mode {
            notes.push("Safe mode: default settings (yours are kept for the next normal start), no recovery offer, bodies meshed on the CPU.".into());
        }
        {
            let rs = painter.render_state();
            let info = rs.adapter.get_info();
            eprintln!("SolidCraft graphics: {:?} {} ({:?}), depth {} bits, MSAA {}x", info.backend, info.name, info.device_type, painter.depth_bits(), painter.msaa_samples());
            solidcraft_engine::crash::phase("viewport pipelines (shader compile)");
            app.set_wgpu(rs, painter.depth_bits(), painter::STENCIL_BITS, painter.msaa_samples());
            solidcraft_ui_egui::gpu::install_picture_renderer(rs);
            solidcraft_engine::crash::set_environment(&format!("Renderer: {:?} {} ({}), MSAA {}x", info.backend, info.name, info.driver, painter.msaa_samples()));
        }
        if options.sample {
            if let Err(e) = app.session.run_script(&solidcraft_engine::sample::script()) {
                app.set_status(format!("sample: {e}"), true);
            }
            app.session.doc_mut().name = "Sample Plate".into();
            app.session.mark_saved();
            app.session.undo.clear();
        }
        for f in &options.files {
            app.open_path_async(f);
        }
        // No file and no sample: the empty design takes Units of new designs (#1205), and the
        // start page shows.
        if options.files.is_empty() && !options.sample {
            solidcraft_ui_egui::prefs::apply_new_design(&mut app);
            let rev = app.session.revision;
            app.home.show(rev);
        }
        // Autosave, and offer what crashed sessions left behind.
        if let Some(dir) = solidcraft_engine::recovery::default_dir() {
            let every = if app.autosave_minutes > 0.0 { app.autosave_minutes * 60.0 } else { f64::MAX / 4.0 };
            solidcraft_ui_egui::recovery_ui::start(&mut app, &dir, Duration::from_secs_f64(every.min(1e9)));
            if safe_mode {
                app.recoverable.clear();
            }
        }
        solidcraft_ui_egui::crash_ui::announce(&mut app, &crashed);
        if !notes.is_empty() {
            app.set_status(notes.join(" "), true);
        }
        solidcraft_engine::crash::phase("first frame");

        let ppp = 1.0f32;
        let (mut width, mut height) = window.inner_size();
        let mut input = crate::input::Input::default();
        let start = Instant::now();
        let mut frame: u64 = 0;
        let mut focused = true;
        let mut busy = false;
        let mut lowered = false;
        let mut title = String::from("SolidCraft");
        let mut prefs_saved = Instant::now();
        let (sw, sh) = window::screen_size();
        let mut code = 0;

        loop {
            // Sleep until something happens or the next repaint is due.
            let due = repaint.due.lock().unwrap_or_else(std::sync::PoisonError::into_inner).take();
            let timeout = match due {
                Some(t) => t.saturating_duration_since(Instant::now()).min(IDLE_POLL),
                None => IDLE_POLL,
            };
            window.wait(timeout);
            while let Some(m) = window.next_message() {
                input.push(window, &m, ppp);
            }
            if std::mem::take(&mut input.resized) {
                (width, height) = window.inner_size();
                painter.resize(width as u32, height as u32);
            }
            if let Some(f) = input.focus.take() {
                focused = f;
            }
            let close_requested = std::mem::take(&mut input.close_requested);

            // This frame's input.
            let screen_rect = Rect::from_min_size(pos2(0.0, 0.0), vec2(width as f32 / ppp, height as f32 / ppp));
            let mut raw = egui::RawInput {
                screen_rect: Some(screen_rect),
                max_texture_side: Some(painter.render_state().device.limits().max_texture_dimension_2d as usize),
                time: Some(start.elapsed().as_secs_f64()),
                predicted_dt: 1.0 / 60.0,
                events: input.take_events(),
                focused,
                ..Default::default()
            };
            if std::mem::take(&mut input.paste_pending) {
                raw.events.push(egui::Event::Paste(window::clipboard_read_text().unwrap_or_default()));
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
            app.raw_input_hook(&mut raw);

            // The frame (what the desktop app's `eframe::App` does around the interface).
            let out = ctx.run_ui(raw, |ui| {
                let ctx = ui.ctx().clone();
                if frame < 2 {
                    ctx.request_repaint();
                    if frame == 1 {
                        solidcraft_engine::crash::phase(solidcraft_engine::crash::RUNNING);
                    }
                }
                app.session.crash_journal = solidcraft_engine::crash::enabled();
                // The close gadget: ask about unsaved designs first; the window closes once
                // they are answered (`quit_requested`).
                if ctx.input(|i| i.viewport().close_requested()) && !app.quit_requested {
                    ctx.send_viewport_cmd(ViewportCommand::CancelClose);
                    solidcraft_ui_egui::documents::request_quit(&mut app);
                }
                app.logic(&ctx);
                if app.quit_requested {
                    ctx.send_viewport_cmd(ViewportCommand::Close);
                }
                app.ui(ui);
            });
            frame += 1;

            // What the frame asked of the platform.
            for cmd in &out.platform_output.commands {
                match cmd {
                    OutputCommand::CopyText(text) => {
                        if !window::clipboard_write_text(text) {
                            log::warn!("could not write the clipboard");
                        }
                    }
                    OutputCommand::CopyImage(_) => log::info!("copying images to the clipboard is not supported on AROS"),
                    OutputCommand::OpenUrl(url) => {
                        if !window::open_url(&url.url) {
                            log::warn!("could not open {}", url.url);
                        }
                    }
                }
            }
            // A file loading on a worker thread: step aside for it (see `sc_set_task_priority`).
            // A task that keeps rendering is not time-sliced against equal-priority threads on
            // every AROS flavour, and the loop only needs to draw when they wait. Equal priority
            // again once it is done.
            let background = app.file_load.busy();
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
                    repaint.ask(Instant::now() + delay);
                }
            }

            // Paint and show.
            let mut textures_delta = out.textures_delta;
            let primitives = ctx.tessellate(out.shapes, out.pixels_per_point);
            let clear = [0.1, 0.1, 0.1, 1.0];
            let capture = options.screenshot.is_some() && frame >= options.screenshot_frames;
            let pixels = painter.paint(clear, &primitives, &mut textures_delta, out.pixels_per_point, capture);
            if capture {
                let path = options.screenshot.as_deref().unwrap_or_default();
                match pixels {
                    Some(rgba) => match image::save_buffer_with_format(path, &rgba, width as u32, height as u32, image::ColorType::Rgba8, image::ImageFormat::Png) {
                        Ok(()) => eprintln!("solidcraft: wrote {path}"),
                        Err(e) => {
                            eprintln!("solidcraft: screenshot {path}: {e}");
                            code = 1;
                        }
                    },
                    None => {
                        eprintln!("solidcraft: screenshot {path}: no frame");
                        code = 1;
                    }
                }
                break;
            }
            if prefs_saved.elapsed() >= PREFS_INTERVAL {
                prefs_saved = Instant::now();
                save_prefs(&app, &prefs_file, safe_mode);
            }
            if closing {
                break;
            }
        }

        // Leaving (the desktop app's `on_exit`): the agent chat's agent and everything it
        // started end with the app, the autosaver stops, the session is closed cleanly.
        solidcraft_ui_egui::agent_chat::shutdown(&mut app);
        solidcraft_ui_egui::recovery_ui::close(&mut app);
        solidcraft_engine::crash::end_session();
        save_prefs(&app, &prefs_file, safe_mode);
        drop(app);
        code
    }

    /// Writes the preferences (never in safe mode, which keeps the user's as they were).
    fn save_prefs(app: &SolveApp, file: &std::path::Path, safe_mode: bool) {
        if safe_mode {
            return;
        }
        if let Err(e) = std::fs::write(file, app.prefs()) {
            log::warn!("cannot write {}: {e}", file.display());
        }
    }
}

#[cfg(target_os = "aros")]
fn main() {
    app::main();
}
