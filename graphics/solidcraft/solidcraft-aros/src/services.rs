//! The platform services the interface asks for on AROS: ASL file requesters for the pickers,
//! the graphics settings file, and nothing for what the system does not have (HTTPS for the
//! update check and the API agents, the keychain, a file manager to show a folder in). The same
//! contract as the desktop app's `services()` (apps/solidcraft/src/main.rs).

use std::cell::RefCell;
use std::path::PathBuf;
use std::rc::Rc;

use solidcraft_engine::io;
use solidcraft_ui_egui::{FilePickerFuture, Services};

use crate::window::{self, Window};

/// The settings directory: `SOLIDCRAFT_DATA_DIR`, else `SolidCraftData` beside the program
/// (`PROGDIR:`), like the desktop app's portable mode. Everything the app persists lives under
/// it (the engine's recovery and crash folders too).
pub fn config_dir() -> PathBuf {
    solidcraft_engine::recovery::data_dir().unwrap_or_else(|| PathBuf::from("PROGDIR:SolidCraftData"))
}

pub fn prefs_file() -> PathBuf {
    config_dir().join("prefs.json")
}

fn graphics_file() -> PathBuf {
    config_dir().join("graphics.json")
}

/// An AmigaDOS pattern matching any of `extensions` (`#?.(step|stp)`); the ASL requester
/// matches it without regard to case.
fn pattern(extensions: &[&str]) -> String {
    let exts: Vec<&str> = extensions.iter().copied().filter(|e| !e.is_empty() && *e != "*").collect();
    if exts.is_empty() {
        return String::new();
    }
    let mut seen = Vec::new();
    for e in exts {
        let lower = e.to_ascii_lowercase();
        if !seen.contains(&lower) {
            seen.push(lower);
        }
    }
    format!("#?.({})", seen.join("|"))
}

/// Everything File › Open reads (the desktop app's list).
fn open_exts() -> Vec<&'static str> {
    vec![io::DESIGN_EXT, io::LEGACY_DESIGN_EXT, "json", "step", "stp", "igs", "iges", "3mf", "stl", "obj", "glb", "gltf", "dxf", "svg"]
}

fn ready(answer: Option<String>) -> FilePickerFuture {
    Box::pin(std::future::ready(answer))
}

/// The services for this shell. The requesters are parented to `window`, shared with the event
/// loop (a requester runs on the event loop's thread, modally, inside the service call).
pub fn aros(window: Rc<Window>) -> Services {
    let last_dir: Rc<RefCell<String>> = Rc::new(RefCell::new(String::new()));
    let pick = {
        let window = window.clone();
        let last_dir = last_dir.clone();
        move |title: &str, pat: &str| -> Option<String> {
            let dir = last_dir.borrow().clone();
            let path = window.file_request(false, title, &dir, "", pat)?;
            *last_dir.borrow_mut() = window::dir_part(&path).to_string();
            Some(path)
        }
    };
    let (pick_open_fn, pick_file_fn, pick_program_fn) = (pick.clone(), pick.clone(), pick);
    let save_window = window;
    let save_last_dir = last_dir;
    Services {
        pick_open: Some(Box::new(move |first: &[&str]| {
            // The file type the command reads comes first, so the requester shows it (#1801).
            let pat = if first.is_empty() { pattern(&open_exts()) } else { pattern(first) };
            ready(pick_open_fn("Open", &pat))
        })),
        pick_file: Some(Box::new(move |label: &str, exts: &[&str]| ready(pick_file_fn(label, &pattern(exts))))),
        pick_program: Some(Box::new(move || ready(pick_program_fn("Choose your slicer", "")))),
        pick_save: Some(Box::new(move |name: &str, exts: &[&str]| {
            let (dir, file) = (window::dir_part(name).to_string(), window::file_part(name).to_string());
            let dir = if dir.is_empty() { save_last_dir.borrow().clone() } else { dir };
            let pat = if exts == [io::DESIGN_EXT, "json"] { String::new() } else { pattern(exts) };
            let path = save_window.file_request(true, "Save As", &dir, &file, &pat);
            if let Some(p) = &path {
                *save_last_dir.borrow_mut() = window::dir_part(p).to_string();
            }
            ready(path)
        })),
        show_in_folder: None,
        graphics_path: Some(graphics_file().to_string_lossy().to_string()),
        http_get: None,
        http_download: None,
        install_app: None,
        agent_http: None,
        secrets: None,
    }
}
