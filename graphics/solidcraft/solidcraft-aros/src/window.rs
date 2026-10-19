//! The Intuition window, through the glue in `aros-gui-sys`: opening, its messages, the
//! timer-backed wait with a wake-up signal for other threads, the title, the busy pointer,
//! the task priority, ASL file requesters and the FTXT clipboard.

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::time::Duration;

use aros_gui_sys as sys;

/// Converts the system charset (ISO 8859-1 on AROS) to UTF-8.
pub fn latin1(bytes: &[u8]) -> String {
    bytes.iter().take_while(|&&b| b != 0).map(|&b| b as char).collect()
}

/// Converts UTF-8 to the system charset, replacing what it cannot represent.
fn to_latin1(s: &str) -> Vec<u8> {
    s.chars().map(|c| if (c as u32) < 256 { c as u32 as u8 } else { b'?' }).collect()
}

fn cstring(s: &str) -> CString {
    // Interior NULs cannot be passed through; the text is cut at the first one.
    CString::new(to_latin1(s.split('\0').next().unwrap_or(""))).unwrap_or_default()
}

unsafe extern "C" {
    fn sc_set_task_priority(pri: i32);
    fn sc_window_set_busy(window: usize, busy: i32);
    fn sc_open_url(url: *const c_char) -> i32;
}

/// The screen's size in pixels.
pub fn screen_size() -> (i32, i32) {
    let (mut w, mut h) = (0, 0);
    // SAFETY: plain out-parameters.
    unsafe { sys::aros_gui_screen_size(&mut w, &mut h) };
    (w.max(640), h.max(480))
}

/// Wakes the event loop from another thread (egui's repaint requests from workers).
#[derive(Clone, Copy)]
pub struct Waker {
    task: usize,
    mask: u32,
}

// SAFETY: Signal() to a task is safe from any task; the task (the event loop's) outlives every
// worker that holds a waker, since the app and its workers are dropped before the window.
unsafe impl Send for Waker {}
unsafe impl Sync for Waker {}

impl Waker {
    pub fn wake(&self) {
        // SAFETY: see the Send impl.
        unsafe { sys::aros_gui_signal_send(self.task as *mut c_void, self.mask) }
    }
}

/// The shell's window. Lives on the event loop's thread; only [`Waker`] crosses threads.
pub struct Window {
    handle: usize,
    info: sys::aros_gui_window_info,
    task: *mut c_void,
    wake_bit: c_int,
}

impl Window {
    pub fn open(width: i32, height: i32, title: &str) -> Option<Self> {
        let title = cstring(title);
        let attrs = sys::aros_gui_window_attrs {
            left: -1,
            top: -1,
            inner_width: width,
            inner_height: height,
            min_width: 640,
            min_height: 400,
            flags: sys::AROS_GUI_WINDOW_RESIZABLE | sys::AROS_GUI_WINDOW_ACTIVATE,
            title: title.as_ptr(),
        };
        let mut info = sys::aros_gui_window_info::default();
        // SAFETY: the attributes and the title outlive the call (the glue copies the title).
        if unsafe { sys::aros_gui_window_open(&attrs, &mut info) } != 0 || info.handle == 0 {
            return None;
        }
        // SAFETY: no arguments.
        let wake_bit = unsafe { sys::aros_gui_signal_alloc() };
        if wake_bit < 0 {
            // SAFETY: the handle was just returned by the glue.
            unsafe { sys::aros_gui_window_close(info.handle) };
            return None;
        }
        // SAFETY: no arguments.
        let task = unsafe { sys::aros_gui_task_self() };
        Some(Self { handle: info.handle, info, task, wake_bit })
    }

    /// The Intuition `struct Window` pointer (the GL surface is made on it).
    pub fn handle(&self) -> usize {
        self.handle
    }

    pub fn waker(&self) -> Waker {
        Waker { task: self.task as usize, mask: 1u32 << self.wake_bit }
    }

    /// The window's current inner size, read back from Intuition (a resize message carries
    /// no geometry).
    pub fn inner_size(&self) -> (i32, i32) {
        let mut info = sys::aros_gui_window_info::default();
        // SAFETY: valid window, plain out-parameter.
        unsafe { sys::aros_gui_window_info(self.handle, &mut info) };
        (info.inner_width.max(1), info.inner_height.max(1))
    }

    /// The next IDCMP message, copied out and replied.
    pub fn next_message(&self) -> Option<sys::aros_gui_message> {
        let mut m = sys::aros_gui_message::default();
        // SAFETY: valid window and a properly laid out message record.
        (unsafe { sys::aros_gui_window_next_message(self.handle, &mut m) } != 0).then_some(m)
    }

    /// Sleeps until a message, a wake-up, or `timeout`.
    pub fn wait(&self, timeout: Duration) {
        let mask = self.info.sigmask | (1u32 << self.wake_bit);
        let us = i64::try_from(timeout.as_micros()).unwrap_or(i64::MAX);
        let mut timed_out = 0;
        // SAFETY: the signal bits are this task's.
        unsafe { sys::aros_gui_wait(mask, us, &mut timed_out) };
    }

    pub fn set_title(&self, title: &str) {
        let title = cstring(title);
        // SAFETY: the glue copies the string before returning.
        unsafe { sys::aros_gui_window_set_title(self.handle, title.as_ptr()) };
    }

    pub fn set_busy(&self, busy: bool) {
        // SAFETY: valid window.
        unsafe { sc_window_set_busy(self.handle, busy as i32) }
    }

    /// The event loop's task priority (see `sc_set_task_priority`).
    pub fn set_priority(&self, pri: i32) {
        // SAFETY: plain call.
        unsafe { sc_set_task_priority(pri) }
    }

    /// Text for a raw key through the current keymap.
    pub fn map_rawkey(&self, code: u16, qualifier: u16) -> String {
        let mut buf = [0 as c_char; 16];
        // SAFETY: the buffer and its length are passed together.
        let n = unsafe { sys::aros_gui_map_rawkey(code, qualifier, buf.as_mut_ptr(), buf.len() as c_int) };
        if n <= 0 {
            return String::new();
        }
        let bytes: Vec<u8> = buf.iter().take(n as usize).map(|&c| c as u8).collect();
        latin1(&bytes)
    }

    /// Shows an ASL file requester (modal) and returns the chosen path.
    pub fn file_request(&self, save: bool, title: &str, drawer: &str, file: &str, pattern: &str) -> Option<String> {
        let (title, drawer, file, pattern) = (cstring(title), cstring(drawer), cstring(file), cstring(pattern));
        let flags = if save { sys::AROS_GUI_FR_SAVE } else { 0 };
        // SAFETY: all strings are NUL-terminated and outlive the call.
        let p = unsafe { sys::aros_gui_file_request(self.handle, title.as_ptr(), drawer.as_ptr(), file.as_ptr(), pattern.as_ptr(), flags) };
        if p.is_null() {
            return None;
        }
        // SAFETY: the glue returns a NUL-terminated string that we free below.
        let path = latin1(unsafe { CStr::from_ptr(p) }.to_bytes());
        // SAFETY: freed exactly once, with the glue's own function.
        unsafe { sys::aros_gui_string_free(p) };
        (!path.is_empty()).then_some(path)
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        // SAFETY: closed exactly once; the signal bit and the timer are this task's.
        unsafe {
            sys::aros_gui_window_close(self.handle);
            sys::aros_gui_signal_free(self.wake_bit);
            sys::aros_gui_wait_cleanup();
        }
    }
}

pub fn open_url(url: &str) -> bool {
    let url = cstring(url);
    // SAFETY: NUL-terminated string.
    unsafe { sc_open_url(url.as_ptr()) != 0 }
}

pub fn clipboard_write_text(text: &str) -> bool {
    let bytes = to_latin1(text);
    // SAFETY: the buffer and its length are passed together.
    unsafe { sys::aros_gui_clip_write(bytes.as_ptr(), bytes.len() as i32) == 0 }
}

pub fn clipboard_read_text() -> Option<String> {
    let mut len = 0i32;
    // SAFETY: plain out-parameter; the text is freed below with the glue's function.
    let p = unsafe { sys::aros_gui_clip_read(&mut len) };
    if p.is_null() {
        return None;
    }
    // SAFETY: the glue returns `len` bytes.
    let text = latin1(unsafe { std::slice::from_raw_parts(p, len.max(0) as usize) });
    // SAFETY: freed exactly once.
    unsafe { sys::aros_gui_clip_free(p) };
    Some(text)
}

/// The last path element of an AmigaDOS or POSIX path.
pub fn file_part(path: &str) -> &str {
    path.rsplit(['/', ':']).next().unwrap_or(path)
}

/// The directory part of an AmigaDOS or POSIX path (`Work:Designs/` for `Work:Designs/a.step`).
pub fn dir_part(path: &str) -> &str {
    match path.rfind(['/', ':']) {
        Some(i) => &path[..=i],
        None => "",
    }
}
