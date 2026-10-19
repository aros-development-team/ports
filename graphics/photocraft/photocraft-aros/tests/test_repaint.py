#!/usr/bin/env python3
"""Run the actual repaint callback on the host with a counted wake signal.

The Intuition wake operation is substituted; deadline handling and callback
registration are extracted unchanged from src/main.rs. No guest I/O is tested.
"""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/main.rs').read_text()
state = source[source.index('struct Repaint {'):source.index('\nfn run(', source.index('struct Repaint {'))]
start = source.index('    let repaint = Arc::new(Repaint')
end = source.index('    let quit = ', start)
registration = source[start:end]
prelude = r'''
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};
mod ffi {
    use super::*;
    #[derive(Clone)]
    pub struct Waker(pub Arc<AtomicUsize>);
    impl Waker { pub fn wake(&self) { self.0.fetch_add(1, Ordering::SeqCst); } }
}
struct Window;
impl Window { fn waker(&self) -> ffi::Waker { ffi::Waker(Arc::new(AtomicUsize::new(0))) } }
struct Request { delay: Duration }
#[derive(Default)]
struct Context { callback: Option<Arc<dyn Fn(Request) + Send + Sync>> }
impl Context {
    fn set_request_repaint_callback(&mut self, f: impl Fn(Request) + Send + Sync + 'static) {
        self.callback = Some(Arc::new(f));
    }
}
'''
tests = r'''
#[test]
fn ui_request_records_deadline_without_waking() {
    let (ctx, repaint) = setup();
    *repaint.due.lock().unwrap() = None;
    let before = Instant::now() + Duration::from_millis(530);
    ctx.callback.unwrap()(Request { delay: Duration::from_millis(530) });
    assert!(repaint.due.lock().unwrap().unwrap() >= before);
    assert_eq!(repaint.waker.0.load(Ordering::SeqCst), 0);
}
#[test]
fn worker_request_wakes_and_records_deadline() {
    let (ctx, repaint) = setup();
    *repaint.due.lock().unwrap() = None;
    std::thread::spawn(move || ctx.callback.unwrap()(Request { delay: Duration::ZERO })).join().unwrap();
    assert!(repaint.due.lock().unwrap().is_some());
    assert_eq!(repaint.waker.0.load(Ordering::SeqCst), 1);
}
#[test]
fn later_request_does_not_postpone_earlier_deadline() {
    let (ctx, repaint) = setup();
    let earlier = Instant::now();
    *repaint.due.lock().unwrap() = Some(earlier);
    ctx.callback.unwrap()(Request { delay: Duration::from_secs(1) });
    assert_eq!(*repaint.due.lock().unwrap(), Some(earlier));
}
'''
with tempfile.TemporaryDirectory(prefix='photocraft-repaint-') as tmp:
    root = Path(tmp)
    rs = root / 'test.rs'
    rs.write_text(prelude + state + '\nfn setup() -> (Context, Arc<Repaint>) {\nlet window = Window;\nlet mut ctx = Context::default();\n' + registration + '\n(ctx, repaint)\n}\n' + tests)
    subprocess.run([os.environ.get('RUSTC', 'rustc'), '--edition=2024', '--test', str(rs), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
