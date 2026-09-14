//! Functional macOS window controls for the frameless Slint title row.
use super::MainWindow;
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use slint::ComponentHandle;
use std::{
    ffi::{CString, c_char, c_void},
    mem,
    time::Duration,
};

#[link(name = "objc")]
unsafe extern "C" {
    fn sel_registerName(name: *const c_char) -> *mut c_void;
    fn objc_msgSend();
}

unsafe fn selector(name: &str) -> *mut c_void {
    let name = CString::new(name).expect("static Objective-C selector");
    unsafe { sel_registerName(name.as_ptr()) }
}

fn with_window(ui: &MainWindow, apply: impl FnOnce(*mut c_void)) -> bool {
    let handle = ui.window().window_handle();
    let Ok(handle) = handle.window_handle() else {
        return false;
    };
    let RawWindowHandle::AppKit(handle) = handle.as_raw() else {
        return false;
    };
    unsafe {
        let get_window: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
            mem::transmute(objc_msgSend as *const ());
        let window = get_window(handle.ns_view.as_ptr(), selector("window"));
        if !window.is_null() {
            apply(window);
            true
        } else {
            false
        }
    }
}

fn configure_window(ui: &MainWindow) -> bool {
    with_window(ui, |window| unsafe {
        let set_bool: unsafe extern "C" fn(*mut c_void, *mut c_void, bool) =
            mem::transmute(objc_msgSend as *const ());
        // Keep the custom Slint title row as the only visible traffic-light
        // surface while allowing it to occupy the native titlebar area.
        set_bool(window, selector("setTitlebarAppearsTransparent:"), true);
        set_bool(window, selector("setHasShadow:"), true);
        set_bool(window, selector("setMovableByWindowBackground:"), true);

        let set_style: unsafe extern "C" fn(*mut c_void, *mut c_void, usize) =
            mem::transmute(objc_msgSend as *const ());
        // Titled | Closable | Miniaturizable | Resizable |
        // FullSizeContentView. The latter keeps Slint's title row under the
        // transparent native titlebar instead of creating two title surfaces.
        set_style(window, selector("setStyleMask:"), 15 | (1usize << 15));

        let standard_button: unsafe extern "C" fn(*mut c_void, *mut c_void, isize) -> *mut c_void =
            mem::transmute(objc_msgSend as *const ());
        let set_hidden: unsafe extern "C" fn(*mut c_void, *mut c_void, bool) =
            mem::transmute(objc_msgSend as *const ());
        // NSWindowButtonClose/Minimize/Zoom = 0/1/2.
        for kind in 0..3 {
            let button = standard_button(window, selector("standardWindowButton:"), kind);
            if !button.is_null() {
                set_hidden(button, selector("setHidden:"), true);
            }
        }

        let set_integer: unsafe extern "C" fn(*mut c_void, *mut c_void, isize) =
            mem::transmute(objc_msgSend as *const ());
        // NSWindowTitleVisibilityHidden = 1.
        set_integer(window, selector("setTitleVisibility:"), 1);
    })
}

fn configure_when_ready(weak: slint::Weak<MainWindow>, attempt: u8) {
    let Some(ui) = weak.upgrade() else {
        return;
    };
    if configure_window(&ui) || attempt >= 40 {
        return;
    }
    slint::Timer::single_shot(Duration::from_millis(25), move || {
        configure_when_ready(weak, attempt + 1);
    });
}

pub(super) fn install(ui: &MainWindow) {
    let weak = ui.as_weak();
    ui.on_native_window_action(move |action| {
        let Some(ui) = weak.upgrade() else {
            return;
        };
        with_window(&ui, |window| unsafe {
            let send: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) =
                mem::transmute(objc_msgSend as *const ());
            let name = match action.as_str() {
                "close" => "performClose:",
                "minimize" => "performMiniaturize:",
                "zoom" => "performZoom:",
                _ => return,
            };
            send(window, selector(name), std::ptr::null_mut());
        });
    });
    configure_when_ready(ui.as_weak(), 0);
}
