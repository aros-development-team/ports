//! Translates the window's events (`ffi::PcEvent`) into egui input, the way egui-winit does for
//! winit: pointer moves and buttons, the wheel, keys with their modifiers, typed text, and the
//! clipboard shortcuts as `Copy` / `Cut` / `Paste` events.

use egui::{Event, Key, Modifiers, MouseWheelUnit, PointerButton, Pos2, TouchPhase, pos2, vec2};

use crate::ffi::{self, PcEvent};

/// What one frame's worth of window events amounts to.
#[derive(Default)]
pub struct Input {
    pub events: Vec<Event>,
    pub modifiers: Modifiers,
    pub pointer: Option<Pos2>,
    /// A new inner size arrived (pixels).
    pub resized: Option<(i32, i32)>,
    pub close_requested: bool,
    pub focus: Option<bool>,
    /// Clipboard text asked for by the app (`ViewportCommand::RequestPaste`), delivered next frame.
    pub paste_pending: bool,
}

impl Input {
    pub fn modifiers_from(qualifier: u32) -> Modifiers {
        let ctrl = qualifier & (ffi::Q_CONTROL | ffi::Q_LCOMMAND | ffi::Q_RCOMMAND) != 0;
        Modifiers {
            alt: qualifier & (ffi::Q_LALT | ffi::Q_RALT) != 0,
            ctrl,
            shift: qualifier & (ffi::Q_LSHIFT | ffi::Q_RSHIFT) != 0,
            mac_cmd: false,
            command: ctrl,
        }
    }

    /// Folds one window event in, in points (`ppp` pixels per point).
    pub fn push(&mut self, ev: &PcEvent, ppp: f32) {
        let mods = Self::modifiers_from(ev.qualifier);
        if mods != self.modifiers {
            self.modifiers = mods;
            self.events.push(Event::ModifiersChanged(mods));
        }
        let pos = pos2(ev.x as f32 / ppp, ev.y as f32 / ppp);
        match ev.kind {
            ffi::EV_CLOSE => self.close_requested = true,
            ffi::EV_RESIZE => self.resized = Some((ev.width, ev.height)),
            ffi::EV_FOCUS => {
                let focused = ev.focus != 0;
                self.focus = Some(focused);
                self.events.push(Event::WindowFocused(focused));
                if !focused {
                    self.events.push(Event::PointerGone);
                    self.pointer = None;
                }
            }
            ffi::EV_MOUSE_MOVE => {
                self.pointer = Some(pos);
                self.events.push(Event::PointerMoved(pos));
            }
            ffi::EV_BUTTON => {
                let button = match ev.button {
                    0 => PointerButton::Primary,
                    1 => PointerButton::Secondary,
                    _ => PointerButton::Middle,
                };
                self.pointer = Some(pos);
                self.events.push(Event::PointerButton { pos, button, pressed: ev.down != 0, modifiers: mods });
            }
            ffi::EV_WHEEL => {
                self.events.push(Event::MouseWheel {
                    unit: MouseWheelUnit::Line,
                    delta: vec2(ev.wheel_x as f32, ev.wheel_y as f32),
                    phase: TouchPhase::Move,
                    modifiers: mods,
                });
            }
            ffi::EV_KEY => self.key(ev, mods),
            _ => {}
        }
    }

    fn key(&mut self, ev: &PcEvent, mods: Modifiers) {
        let pressed = ev.down != 0;
        let repeat = ev.qualifier & ffi::Q_REPEAT != 0;
        let plain = ev.plain();
        let key = key_for(ev.rawkey, &plain);
        // The clipboard shortcuts become egui's clipboard events (as egui-winit does), with the
        // text read here for Paste; the app's raw input hook knows how to give them back as key
        // presses outside text fields.
        if pressed && mods.command && !mods.alt && !mods.shift {
            match key {
                Some(Key::C) => {
                    self.events.push(Event::Copy);
                    return;
                }
                Some(Key::X) => {
                    self.events.push(Event::Cut);
                    return;
                }
                Some(Key::V) => {
                    self.events.push(Event::Paste(ffi::clipboard_read_text().unwrap_or_default()));
                    return;
                }
                _ => {}
            }
        }
        if let Some(key) = key {
            self.events.push(Event::Key { key, physical_key: None, pressed, repeat, modifiers: mods });
        }
        if pressed && !mods.ctrl && !mods.command {
            let text: String = ev.text().chars().filter(|c| !c.is_control()).collect();
            if !text.is_empty() {
                self.events.push(Event::Text(text));
            }
        }
    }

    /// Takes the frame's events, leaving the persistent state (modifiers, pointer).
    pub fn take_events(&mut self) -> Vec<Event> {
        std::mem::take(&mut self.events)
    }

}

/// The logical key for a raw key code: the special keys by code (they are the same on every
/// keymap), the others from the character the keymap gives the unshifted key.
fn key_for(rawkey: i32, plain: &str) -> Option<Key> {
    let special = match rawkey {
        0x41 => Some(Key::Backspace),
        0x42 => Some(Key::Tab),
        0x43 => Some(Key::Enter), // keypad Enter
        0x44 => Some(Key::Enter),
        0x45 => Some(Key::Escape),
        0x46 => Some(Key::Delete),
        0x47 => Some(Key::Insert),
        0x48 => Some(Key::PageUp),
        0x49 => Some(Key::PageDown),
        0x4c => Some(Key::ArrowUp),
        0x4d => Some(Key::ArrowDown),
        0x4e => Some(Key::ArrowRight),
        0x4f => Some(Key::ArrowLeft),
        0x50 => Some(Key::F1),
        0x51 => Some(Key::F2),
        0x52 => Some(Key::F3),
        0x53 => Some(Key::F4),
        0x54 => Some(Key::F5),
        0x55 => Some(Key::F6),
        0x56 => Some(Key::F7),
        0x57 => Some(Key::F8),
        0x58 => Some(Key::F9),
        0x59 => Some(Key::F10),
        0x4b => Some(Key::F11),
        0x6f => Some(Key::F12),
        0x5f => Some(Key::Insert), // Help
        0x70 => Some(Key::Home),
        0x71 => Some(Key::End),
        0x40 => Some(Key::Space),
        _ => None,
    };
    if special.is_some() {
        return special;
    }
    let c = plain.chars().next()?;
    if c.is_control() || c == ' ' {
        return None;
    }
    let name: String = c.to_uppercase().collect();
    Key::from_name(&name)
}
