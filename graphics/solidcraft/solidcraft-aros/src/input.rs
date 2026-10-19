//! Translates the window's IDCMP messages into egui input, the way egui-winit does for winit:
//! pointer moves and buttons, the wheel (NewMouse raw keys), keys with their modifiers, typed
//! text, and the clipboard shortcuts as `Copy` / `Cut` / `Paste` events.

use aros_gui_sys as sys;
use egui::{Event, Key, Modifiers, MouseWheelUnit, PointerButton, Pos2, TouchPhase, pos2, vec2};

use crate::window::{self, Window};

/// NewMouse wheel codes (devices/rawkeycodes.h).
const RAWKEY_NM_WHEEL_UP: u16 = 0x7A;
const RAWKEY_NM_WHEEL_DOWN: u16 = 0x7B;
const RAWKEY_NM_WHEEL_LEFT: u16 = 0x7C;
const RAWKEY_NM_WHEEL_RIGHT: u16 = 0x7D;

/// What one frame's worth of window messages amounts to.
#[derive(Default)]
pub struct Input {
    pub events: Vec<Event>,
    pub modifiers: Modifiers,
    pub pointer: Option<Pos2>,
    /// The window's size changed (the caller re-reads it).
    pub resized: bool,
    pub close_requested: bool,
    pub focus: Option<bool>,
    /// Clipboard text asked for by the app (`ViewportCommand::RequestPaste`), delivered next frame.
    pub paste_pending: bool,
}

impl Input {
    pub fn modifiers_from(qualifier: u16) -> Modifiers {
        let ctrl = qualifier & (sys::IEQUALIFIER_CONTROL | sys::IEQUALIFIER_LCOMMAND | sys::IEQUALIFIER_RCOMMAND) != 0;
        Modifiers {
            alt: qualifier & (sys::IEQUALIFIER_LALT | sys::IEQUALIFIER_RALT) != 0,
            ctrl,
            shift: qualifier & (sys::IEQUALIFIER_LSHIFT | sys::IEQUALIFIER_RSHIFT) != 0,
            mac_cmd: false,
            command: ctrl,
        }
    }

    /// Folds one window message in, in points (`ppp` pixels per point).
    pub fn push(&mut self, window: &Window, m: &sys::aros_gui_message, ppp: f32) {
        let mods = Self::modifiers_from(m.qualifier);
        if mods != self.modifiers {
            self.modifiers = mods;
            self.events.push(Event::ModifiersChanged(mods));
        }
        let pos = pos2(m.mouse_x as f32 / ppp, m.mouse_y as f32 / ppp);
        match m.class {
            sys::IDCMP_CLOSEWINDOW => self.close_requested = true,
            sys::IDCMP_NEWSIZE | sys::IDCMP_CHANGEWINDOW => self.resized = true,
            sys::IDCMP_ACTIVEWINDOW | sys::IDCMP_INACTIVEWINDOW => {
                let focused = m.class == sys::IDCMP_ACTIVEWINDOW;
                self.focus = Some(focused);
                self.events.push(Event::WindowFocused(focused));
                if !focused {
                    self.events.push(Event::PointerGone);
                    self.pointer = None;
                }
            }
            sys::IDCMP_MOUSEMOVE => {
                self.pointer = Some(pos);
                self.events.push(Event::PointerMoved(pos));
            }
            sys::IDCMP_MOUSEBUTTONS => {
                let button = match m.code & !sys::IECODE_UP_PREFIX {
                    sys::IECODE_LBUTTON => PointerButton::Primary,
                    sys::IECODE_RBUTTON => PointerButton::Secondary,
                    sys::IECODE_MBUTTON => PointerButton::Middle,
                    _ => return,
                };
                let pressed = m.code & sys::IECODE_UP_PREFIX == 0;
                self.pointer = Some(pos);
                self.events.push(Event::PointerButton { pos, button, pressed, modifiers: mods });
            }
            sys::IDCMP_RAWKEY => {
                let code = m.code & !sys::IECODE_UP_PREFIX;
                let pressed = m.code & sys::IECODE_UP_PREFIX == 0;
                match code {
                    RAWKEY_NM_WHEEL_UP..=RAWKEY_NM_WHEEL_RIGHT => {
                        if pressed {
                            let delta = match code {
                                RAWKEY_NM_WHEEL_UP => vec2(0.0, 1.0),
                                RAWKEY_NM_WHEEL_DOWN => vec2(0.0, -1.0),
                                RAWKEY_NM_WHEEL_LEFT => vec2(1.0, 0.0),
                                _ => vec2(-1.0, 0.0),
                            };
                            self.events.push(Event::MouseWheel { unit: MouseWheelUnit::Line, delta, phase: TouchPhase::Move, modifiers: mods });
                        }
                    }
                    0x60..=0x67 => {} // the modifier keys themselves: ModifiersChanged covers them
                    _ => self.key(window, code, pressed, m.qualifier, mods),
                }
            }
            _ => {}
        }
    }

    fn key(&mut self, window: &Window, code: u16, pressed: bool, qualifier: u16, mods: Modifiers) {
        let repeat = qualifier & sys::IEQUALIFIER_REPEAT != 0;
        // The key with no qualifiers held names the logical key; the keymap's text is what is typed.
        let plain = window.map_rawkey(code, qualifier & sys::IEQUALIFIER_NUMERICPAD);
        let key = key_for(code, &plain);
        // The clipboard shortcuts become egui's clipboard events (as egui-winit does), with the
        // text read here for Paste.
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
                    self.events.push(Event::Paste(window::clipboard_read_text().unwrap_or_default()));
                    return;
                }
                _ => {}
            }
        }
        if let Some(key) = key {
            self.events.push(Event::Key { key, physical_key: None, pressed, repeat, modifiers: mods });
        }
        if pressed && !mods.ctrl && !mods.command {
            let text: String = window.map_rawkey(code, qualifier).chars().filter(|c| !c.is_control()).collect();
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
fn key_for(rawkey: u16, plain: &str) -> Option<Key> {
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
