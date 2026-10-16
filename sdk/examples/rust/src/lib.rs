//! «Дыхание света»: a CellFap native mod in Rust (docs/mods.md, «Rust»).
//!
//! Only the C ABI of sdk/cellfap_mod.h is used, declared by hand below (no bindgen, no other crates). The structs
//! must keep the header's layout: fields in the same order, `#[repr(C)]`. Functions of CellFapApi this mod does not
//! call are kept as plain pointers, so the offsets stay right.
//!
//! Build: `cargo build --release` -> target/release/cellfap_rust_example.dll; for the phone
//! `cargo ndk -t arm64-v8a build --release` -> libcellfap_rust_example.so. Put it into native/<platform>/ next to mod.txt.
#![allow(non_camel_case_types, dead_code)]

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::Mutex;

pub const CELLFAP_MOD_API_VERSION: u32 = 1;
pub const CELLFAP_OK: c_int = 0;
pub const CELLFAP_ERROR: c_int = -1;

#[repr(C)]
pub struct CellFapWorld {
    _opaque: [u8; 0],
}
#[repr(C)]
pub struct CellFapUi {
    _opaque: [u8; 0],
}
#[repr(C)]
pub struct CellFapDraw {
    _opaque: [u8; 0],
}

#[repr(C)]
#[derive(Default)]
pub struct CellFapWorldInfo {
    pub ticks: u64,
    pub hours: f64,
    pub cells: i32,
    pub cell_slots: i32,
    pub link_slots: i32,
    pub food: i32,
    pub genomes: i32,
    pub cell_type_count: i32,
    pub dish_radius: f32,
    pub daylight: f32,
    pub max_cells: i32,
    pub max_food: i32,
    pub reserved: [i32; 8],
}

#[repr(C)]
#[derive(Default)]
pub struct CellFapCell {
    pub id: u32,
    pub alive: i32,
    pub type_: i32,
    pub mode: i32,
    pub genome: i32,
    pub link_count: i32,
    pub radius: f32,
    pub actuator: f32,
    pub x: f32,
    pub y: f32,
    pub vx: f32,
    pub vy: f32,
    pub angle: f32,
    pub angular_velocity: f32,
    pub mass: f32,
    pub age: f32,
    pub nitrogen: f32,
    pub injury: f32,
    pub tag: i32,
    pub mirrored: i32,
    pub telomeres: i32,
    pub mutations: i32,
    pub signals: [f32; 8],
    pub charge: f32,
    pub stun: f32,
    pub paralysis: f32,
    pub stimulant: f32,
    pub substances: [f32; 15],
    pub memory: [f32; 4],
    pub reserved: [f32; 8],
}

type Unused = *const c_void;

#[repr(C)]
pub struct CellFapApi {
    pub api_version: u32,
    pub struct_size: u32,
    pub log: unsafe extern "C" fn(*mut CellFapMod, *const c_char),
    pub message: unsafe extern "C" fn(*mut CellFapMod, *const c_char),
    pub set_error: unsafe extern "C" fn(*mut CellFapMod, *const c_char),
    pub setting: unsafe extern "C" fn(*mut CellFapMod, *const c_char, f64) -> f64,
    pub set_setting: Unused,
    pub mod_file: Unused,
    pub register_genome: Unused,
    pub register_program: Unused,
    pub request_dish: Unused,
    pub cell_type_name: unsafe extern "C" fn(i32) -> *const c_char,
    pub param_name: Unused,
    pub world_info: unsafe extern "C" fn(*mut CellFapWorld, *mut CellFapWorldInfo) -> c_int,
    pub get_param: unsafe extern "C" fn(*mut CellFapWorld, *const c_char, *mut f64) -> c_int,
    pub set_param: unsafe extern "C" fn(*mut CellFapWorld, *const c_char, f64) -> c_int,
    pub get_cell: unsafe extern "C" fn(*mut CellFapWorld, i32, *mut CellFapCell) -> c_int,
    // set_cell .. edit_cell_mode: 23 functions this mod does not call
    pub unused_world: [Unused; 23],
    pub ui_text: unsafe extern "C" fn(*mut CellFapUi, *const c_char),
    pub ui_text_colored: Unused,
    pub ui_heading: unsafe extern "C" fn(*mut CellFapUi, *const c_char),
    pub unused_ui: [Unused; 7],
    pub unused_draw: [Unused; 3],
}

type Cb<T> = Option<T>;

#[repr(C)]
pub struct CellFapMod {
    pub struct_size: u32,
    pub api_version: u32,
    pub id: *const c_char,
    pub lib_dir: *const c_char,
    pub data_dir: *const c_char,
    pub platform: *const c_char,
    pub host: *mut c_void,
    pub user: *mut c_void,
    pub panel_title: *const c_char,
    pub cell_types: u64,
    pub events: u32,
    pub flags: u32,
    pub unload: Cb<unsafe extern "C" fn(*mut CellFapMod) -> c_int>,
    pub before_step: Cb<unsafe extern "C" fn(*mut CellFapMod, *mut CellFapWorld, *mut c_int) -> c_int>,
    pub after_step: Cb<unsafe extern "C" fn(*mut CellFapMod, *mut CellFapWorld) -> c_int>,
    pub cell_update: Cb<unsafe extern "C" fn(*mut CellFapMod, *mut CellFapWorld, i32) -> c_int>,
    pub cell_event: Cb<unsafe extern "C" fn(*mut CellFapMod, *mut CellFapWorld, *const c_void) -> c_int>,
    pub world_reset: Cb<unsafe extern "C" fn(*mut CellFapMod, *mut CellFapWorld) -> c_int>,
    pub settings_changed: Cb<unsafe extern "C" fn(*mut CellFapMod, *mut CellFapWorld) -> c_int>,
    pub panel: Cb<unsafe extern "C" fn(*mut CellFapMod, *mut CellFapWorld, *mut CellFapUi) -> c_int>,
    pub draw: Cb<unsafe extern "C" fn(*mut CellFapMod, *mut CellFapWorld, *mut CellFapDraw) -> c_int>,
    pub reserved: [*mut c_void; 16],
}

// ---------------------------------------------------------------- the mod

static API: AtomicPtr<CellFapApi> = AtomicPtr::new(std::ptr::null_mut());

struct State {
    base_light: f64, // the dish's light before the mod touched it
}
static STATE: Mutex<State> = Mutex::new(State { base_light: 1.0 });

fn api() -> &'static CellFapApi {
    // Set once in cellfap_mod_load, valid until the library is unloaded.
    unsafe { &*API.load(Ordering::Acquire) }
}

fn cstr(s: &str) -> CString {
    CString::new(s.replace('\0', " ")).unwrap()
}

unsafe fn log(m: *mut CellFapMod, text: &str) {
    (api().log)(m, cstr(text).as_ptr());
}

unsafe fn setting(m: *mut CellFapMod, key: &str, fallback: f64) -> f64 {
    (api().setting)(m, cstr(key).as_ptr(), fallback)
}

unsafe extern "C" fn world_reset(m: *mut CellFapMod, w: *mut CellFapWorld) -> c_int {
    let mut light = 1.0;
    if (api().get_param)(w, cstr("light").as_ptr(), &mut light) != CELLFAP_OK {
        return CELLFAP_ERROR;
    }
    STATE.lock().unwrap().base_light = light;
    log(m, &format!("свет чашки: {light:.2}"));
    CELLFAP_OK
}

unsafe extern "C" fn after_step(m: *mut CellFapMod, w: *mut CellFapWorld) -> c_int {
    let mut info = CellFapWorldInfo::default();
    if (api().world_info)(w, &mut info) != CELLFAP_OK {
        return CELLFAP_ERROR;
    }
    let period = setting(m, "период", 2.0).max(0.1);
    let depth = setting(m, "глубина", 0.6).clamp(0.0, 1.0);
    let phase = (info.hours / period * std::f64::consts::TAU).cos();
    let base = STATE.lock().unwrap().base_light;
    let light = base * (1.0 - depth * 0.5 * (1.0 - phase));
    (api().set_param)(w, cstr("light").as_ptr(), light);
    CELLFAP_OK
}

unsafe extern "C" fn panel(_m: *mut CellFapMod, w: *mut CellFapWorld, ui: *mut CellFapUi) -> c_int {
    let a = api();
    let mut info = CellFapWorldInfo::default();
    if w.is_null() || (a.world_info)(w, &mut info) != CELLFAP_OK {
        return CELLFAP_OK;
    }
    // Cells of each type.
    let mut counts = vec![0u32; info.cell_type_count.max(0) as usize];
    let mut c = CellFapCell::default();
    for i in 0..info.cell_slots {
        if (a.get_cell)(w, i, &mut c) == CELLFAP_OK && c.alive != 0 {
            if let Some(n) = counts.get_mut(c.type_ as usize) {
                *n += 1;
            }
        }
    }
    let mut light = 0.0;
    (a.get_param)(w, cstr("light").as_ptr(), &mut light);
    (a.ui_text)(ui, cstr(&format!("Часы: {:.2} · свет сейчас: {:.2}", info.hours, light)).as_ptr());
    (a.ui_heading)(ui, cstr("Клетки по типам").as_ptr());
    for (t, n) in counts.iter().enumerate().filter(|(_, n)| **n > 0) {
        let name = CStr::from_ptr((a.cell_type_name)(t as i32)).to_string_lossy();
        (a.ui_text)(ui, cstr(&format!("{name}: {n}")).as_ptr());
    }
    CELLFAP_OK
}

unsafe extern "C" fn unload(m: *mut CellFapMod) -> c_int {
    log(m, "Rust-мод выгружен");
    CELLFAP_OK
}

#[no_mangle]
pub extern "C" fn cellfap_mod_api_version() -> u32 {
    CELLFAP_MOD_API_VERSION
}

#[no_mangle]
pub unsafe extern "C" fn cellfap_mod_load(a: *const CellFapApi, m: *mut CellFapMod) -> c_int {
    if a.is_null() || m.is_null() || (*a).api_version < 1 || ((*a).struct_size as usize) < std::mem::size_of::<CellFapApi>() {
        return CELLFAP_ERROR;
    }
    API.store(a as *mut CellFapApi, Ordering::Release);
    static TITLE: &[u8] = "Дыхание света\0".as_bytes();
    let md = &mut *m;
    md.panel_title = TITLE.as_ptr() as *const c_char;
    md.world_reset = Some(world_reset);
    md.after_step = Some(after_step);
    md.panel = Some(panel);
    md.unload = Some(unload);
    let platform = CStr::from_ptr(md.platform).to_string_lossy();
    log(m, &format!("Rust-мод загружен ({platform})"));
    CELLFAP_OK
}
