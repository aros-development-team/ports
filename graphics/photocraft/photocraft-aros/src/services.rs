//! The platform services the UI shell needs on AROS: file requesters (asl.library), the
//! filesystem, codecs, preferences, crash recovery. The same contract as the desktop app's
//! `services.rs` (apps/photocraft/src/services.rs, which this follows), without rfd, arboard
//! and the other crates that have no AROS backend.

use std::cell::{Cell, RefCell};
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use photocraft_codecs::{ChannelLayout, EncodeOptions, Image};
use photocraft_doc::Document;
use photocraft_format::RecoveryStore;
use photocraft_ui_egui::{FileDialogAnswer, FileDialogReply, FileDialogRequest, Recoverable, Services};

use crate::ffi;

/// Everything File › Open reads (the desktop app's list).
const OPEN_EXTS: &[&str] = &[
    "pcraft", "pdn", "ora", "psd", "psb", "psdt", "png", "jpg", "jpeg", "tif", "tiff", "webp", "gif", "bmp", "tga", "ico", "qoi", "exr", "hdr", "pbm", "pgm",
    "ppm", "pam", "pfm", "fits", "fit", "fts", "heic", "heif", "hif", "dng", "cr2", "cr3", "nef", "nrw", "arw", "pef", "orf", "rw2", "raf", "abr", "grd",
    "svg", "svgz", "aco", "ase", "kys", "af", "afdesign", "afphoto", "afpub",
];

/// An AmigaDOS pattern matching any of `extensions` (`#?.(psd|png)`); the ASL requester matches
/// it without regard to case.
fn pattern(extensions: &[&str]) -> String {
    if extensions.is_empty() {
        return String::new();
    }
    format!("#?.({})", extensions.join("|"))
}

/// The settings directory: `PHOTOCRAFT_CONFIG_DIR`, else `PhotoCraftData` beside the program
/// (`PROGDIR:`), like the desktop app's portable mode. Everything the app persists lives under it.
pub fn config_dir() -> Option<PathBuf> {
    if let Some(d) = std::env::var_os("PHOTOCRAFT_CONFIG_DIR").filter(|d| !d.is_empty()) {
        return Some(PathBuf::from(d));
    }
    Some(PathBuf::from("PROGDIR:PhotoCraftData"))
}

pub fn prefs_file() -> Option<PathBuf> {
    config_dir().map(|d| d.join("preferences.json"))
}

pub fn presets_dir() -> Option<PathBuf> {
    config_dir().map(|d| d.join("Presets"))
}

fn recovery_dir() -> Option<PathBuf> {
    config_dir().map(|d| d.join("Recovery"))
}

/// Write `bytes` crash-safely (temporary file beside the target, sync, rename).
fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), String> {
    photocraft_format::atomic_write(path, bytes).map_err(|e| e.to_string())
}

fn export_options(settings: &photocraft_ui_egui::ExportSettings) -> photocraft_io::ExportOptions {
    let mut opts = photocraft_io::ExportOptions::default();
    if let Some(q) = settings.jpeg_quality {
        opts.encode.jpeg_quality = q;
    }
    opts.encode.webp_lossless = settings.webp_lossless;
    if let Some(q) = settings.webp_quality {
        opts.encode.webp_quality = q;
    }
    opts.tiff_layers = settings.tiff_layers;
    opts.xmp = if settings.xmp_all { photocraft_io::XmpEmbed::All } else { photocraft_io::XmpEmbed::None };
    opts.tga_bits = settings.tga_bits;
    opts.image_previews = settings.image_previews;
    opts
}

/// A save on a worker thread: encode, then write through `write`.
fn save_file(
    doc: &Document,
    path: &str,
    settings: &photocraft_ui_egui::ExportSettings,
    ctl: &photocraft_ui_egui::jobs_ui::SaveCtl,
) -> Result<Vec<String>, String> {
    ctl.progress(0.0, "Encoding");
    let opts = export_options(settings);
    let r = photocraft_io::export(doc, path, &opts).map_err(|e| e.to_string())?;
    ctl.commit()?;
    ctl.progress(0.0, "Writing");
    write_atomic(Path::new(path), &r.bytes)?;
    Ok(r.warnings)
}

type SharedRecovery = Rc<RefCell<Option<RecoveryStore>>>;

fn with_store<R>(store: &SharedRecovery, f: impl FnOnce(&mut RecoveryStore) -> R) -> Result<R, String> {
    let mut slot = store.try_borrow_mut().map_err(|_| "crash recovery is busy".to_string())?;
    Ok(f(slot.as_mut().ok_or("no config directory")?))
}

/// Crash recovery: background incremental .pcraft autosaves into `dir`.
fn recovery_services(dir: Option<PathBuf>) -> Services {
    let recover_dir = dir.clone();
    let store: SharedRecovery = Rc::new(RefCell::new(dir.map(RecoveryStore::new)));
    let (s1, s2, s4) = (store.clone(), store.clone(), store.clone());
    Services {
        autosave: Some(Box::new(move |doc: &Arc<Document>, revision: u64, path: Option<&str>| {
            with_store(&s1, |s| s.autosave_checked(doc, revision, path.map(str::to_string)))?
        })),
        autosave_results: Some(Box::new(move || with_store(&s4, |s| s.take_completed()).unwrap_or_default())),
        discard_autosave: Some(Box::new(move |id: u64| {
            let _ = with_store(&s2, |s| s.discard(id));
        })),
        recover: Some(Box::new(move || {
            let Some(dir) = recover_dir.as_ref() else { return Vec::new() };
            photocraft_format::list_recovery(dir)
                .into_iter()
                .map(|e| Recoverable {
                    key: e.info.key.clone(),
                    name: e.info.document_name.clone(),
                    path: e.info.original_path.clone(),
                    load: Box::new(move || photocraft_format::recover(&e).map_err(|e| e.to_string())),
                })
                .collect()
        })),
        adopt_autosave: Some(Box::new(move |id: u64, key: &str| {
            let _ = with_store(&store, |s| s.adopt(id, key));
        })),
        ..Default::default()
    }
}

/// The window the requesters are parented to, shared with the event loop (the dialog runs on
/// the main thread, modally, while the loop is inside the service call).
pub type SharedWindow = Rc<RefCell<Option<Rc<ffi::Window>>>>;

/// Shows an ASL requester for `request` and answers at once.
fn show_file_dialog(window: &SharedWindow, last_dir: &Rc<RefCell<String>>, request: FileDialogRequest, reply: FileDialogReply) {
    let win = window.borrow().clone();
    let Some(win) = win else {
        reply.send(None);
        return;
    };
    let answer = match request {
        FileDialogRequest::Open { multiple, initial_dir, extensions } => {
            let dir = initial_dir.unwrap_or_else(|| last_dir.borrow().clone());
            let pat = match extensions {
                Some(exts) => pattern(&exts.iter().map(String::as_str).collect::<Vec<_>>()),
                None => pattern(OPEN_EXTS),
            };
            let paths = win.file_request(false, multiple, "Open", &dir, "", &pat);
            if let Some(first) = paths.first() {
                *last_dir.borrow_mut() = ffi::dir_part(first).to_string();
            }
            (!paths.is_empty()).then_some(FileDialogAnswer::Paths(paths))
        }
        FileDialogRequest::Save { suggested } => {
            let (dir, file) = (ffi::dir_part(&suggested).to_string(), ffi::file_part(&suggested).to_string());
            let dir = if dir.is_empty() { last_dir.borrow().clone() } else { dir };
            let paths = win.file_request(true, false, "Save As", &dir, &file, "");
            match paths.into_iter().next() {
                Some(p) => {
                    *last_dir.borrow_mut() = ffi::dir_part(&p).to_string();
                    Some(FileDialogAnswer::SaveTo(p))
                }
                None => None,
            }
        }
    };
    reply.send(answer);
}

/// The services for this shell. `quit` is set when the app decides to leave; `caps_lock` reads
/// the window's last qualifiers.
pub fn aros(window: SharedWindow, quit: Rc<Cell<bool>>) -> Services {
    let last_dir: Rc<RefCell<String>> = Rc::new(RefCell::new(String::new()));
    let dialog_window = window.clone();
    let caps_window = window;
    Services {
        import: Some(Box::new(|name: &str, bytes: &[u8], max_svg_group_depth: usize| {
            photocraft_io::import_with_svg_group_depth(name, bytes, max_svg_group_depth).map(|r| (r.document, r.warnings)).map_err(|e| e.to_string())
        })),
        export: Some(Box::new(|doc: &Document, path: &str, settings: &photocraft_ui_egui::ExportSettings| {
            let opts = export_options(settings);
            photocraft_io::export(doc, path, &opts).map(|r| (r.bytes, r.warnings)).map_err(|e| e.to_string())
        })),
        save_file: Some(Arc::new(save_file)),
        file_dialog: Some(Box::new(move |request, _parent, reply| show_file_dialog(&dialog_window, &last_dir, request, reply))),
        write: Some(Box::new(|path: &str, bytes: &[u8]| write_atomic(Path::new(path), bytes))),
        encode_png: Some(Box::new(|w, h, rgba| {
            let img = Image::from_u8(w, h, ChannelLayout::Rgba, rgba.to_vec()).map_err(|e| e.to_string())?;
            photocraft_codecs::encode(&img, photocraft_codecs::Format::Png, &EncodeOptions::default()).map_err(|e| e.to_string())
        })),
        open_url: Some(Box::new(|url: &str| if ffi::open_url(url) { Ok(()) } else { Err("OpenURL is not available".to_string()) })),
        beep: Some(ffi::beep),
        load_prefs: Some(Box::new(|| std::fs::read_to_string(prefs_file()?).ok())),
        save_prefs: Some(Box::new(|text: &str| write_atomic(&prefs_file().ok_or("no config directory")?, text.as_bytes()))),
        append_text: Some(Box::new(|path: &str, text: &str| {
            use std::io::Write;
            let mut f = std::fs::OpenOptions::new().create(true).append(true).open(path).map_err(|e| e.to_string())?;
            f.write_all(text.as_bytes()).map_err(|e| e.to_string())
        })),
        caps_lock: Some(Box::new(move || caps_window.borrow().as_ref().is_some_and(|w| w.qualifier() & ffi::Q_CAPSLOCK != 0))),
        quit: Some(Box::new(move || quit.set(true))),
        ..recovery_services(recovery_dir())
    }
}
