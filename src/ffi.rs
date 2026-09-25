//! C ABI для прослойки `shim/plugin.cpp`. Описание — `include/noshare_cover.h`.
//!
//! Правила:
//! - ни одна паника не выходит наружу (`guard`), иначе упадёт весь Hyprland;
//! - всё состояние — один `Registry` под мьютексом; вызовы идут из потока
//!   рендера, мьютекс тут дешёвый и защищает от гонок с потоками декода;
//! - указатели в `nsc_frame` живут до `nsc_end_frame` того же кадра.

use std::ffi::{CStr, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::{Mutex, MutexGuard};
use std::time::Instant;

use crate::config::{RawSettings, Settings};
use crate::extra;
use crate::frame::{DRM_FORMAT_ARGB8888, FrameData};
use crate::registry::Registry;

static STATE: Mutex<Option<Registry>> = Mutex::new(None);

fn state() -> MutexGuard<'static, Option<Registry>> {
    STATE.lock().unwrap_or_else(|e| e.into_inner())
}

fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(fallback)
}

/// NULL и не-UTF-8 превращаются в пустую строку — это «не задано».
unsafe fn str_or_empty<'a>(p: *const c_char) -> &'a str {
    if p.is_null() {
        return "";
    }
    unsafe { CStr::from_ptr(p) }.to_str().unwrap_or("")
}

unsafe fn opt_str<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        None
    } else {
        unsafe { CStr::from_ptr(p) }.to_str().ok()
    }
}

#[repr(C)]
pub struct NscSettings {
    pub path_cover: *const c_char,
    pub looped: bool,
    pub speed: f64,
    pub backend: *const c_char,
    pub gpu_device: *const c_char,
}

/// Переопределения из правила окна. NULL — поле в правиле не задано.
#[repr(C)]
pub struct NscPlayRequest {
    pub rule_path: *const c_char,
    pub rule_speed: *const c_char,
    pub rule_loop: *const c_char,
}

pub const NSC_FRAME_NONE: u32 = 0;
pub const NSC_FRAME_CPU: u32 = 1;
pub const NSC_FRAME_DMABUF: u32 = 2;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NscPlane {
    pub fd: i32,
    pub offset: u32,
    pub stride: u32,
}

#[repr(C)]
pub struct NscFrame {
    pub cover_id: u64,
    pub generation: u64,
    pub kind: u32,
    pub width: u32,
    pub height: u32,
    /// CPU: байт на строку, pixels — premultiplied BGRA (`fourcc` = ARGB8888).
    pub stride: u32,
    pub pixels: *const u8,
    pub fourcc: u32,
    pub modifier: u64,
    pub plane_count: u32,
    pub planes: [NscPlane; 4],
}

impl NscFrame {
    fn empty() -> Self {
        Self {
            cover_id: 0,
            generation: 0,
            kind: NSC_FRAME_NONE,
            width: 0,
            height: 0,
            stride: 0,
            pixels: std::ptr::null(),
            fourcc: 0,
            modifier: 0,
            plane_count: 0,
            planes: [NscPlane::default(); 4],
        }
    }
}

/// Прямоугольник от другого плагина, как его рисует прослойка.
#[repr(C)]
pub struct NscExtraRect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub rounding: f64,
    /// адрес окна Hyprland для заливки его обложкой, 0 — нет
    pub window: u64,
    /// 0 — чёрный, 1 — обложка окна `window`
    pub fill: u32,
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_init() -> bool {
    guard(false, || {
        *state() = Some(Registry::default());
        true
    })
}

/// Выгрузка: закрыть все источники (join потоков видео), очистить extra rects.
#[unsafe(no_mangle)]
pub extern "C" fn nsc_shutdown() {
    guard((), || {
        let reg = state().take();
        drop(reg);
        extra::reset();
    })
}

/// # Safety
/// `s` — валидный указатель на `NscSettings` на время вызова (или NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_set_settings(s: *const NscSettings) {
    guard((), || {
        let Some(s) = (unsafe { s.as_ref() }) else {
            return;
        };
        let raw = RawSettings {
            path_cover: unsafe { str_or_empty(s.path_cover) },
            looped: s.looped,
            speed: s.speed,
            backend: unsafe { str_or_empty(s.backend) },
            gpu_device: unsafe { str_or_empty(s.gpu_device) },
        };
        let (settings, err) = Settings::from_raw(&raw);
        if let Some(reg) = state().as_mut() {
            reg.set_settings(settings, err);
        }
    })
}

/// Растёт при каждом сбросе обложек. Сменилась — выкинуть все свои текстуры.
#[unsafe(no_mangle)]
pub extern "C" fn nsc_epoch() -> u64 {
    guard(0, || state().as_ref().map_or(0, Registry::epoch))
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_begin_frame() {
    guard((), || {
        if let Some(reg) = state().as_mut() {
            reg.begin_frame();
        }
    })
}

/// Обложка для окна. `false` — рисовать нечего.
///
/// # Safety
/// `req` и `out` — валидные указатели на время вызова. Данные в `out`
/// (pixels, fd) действительны до `nsc_end_frame`; fd не закрывать и не
/// передавать во владение — при импорте в EGL его нужно `dup()`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_resolve(req: *const NscPlayRequest, out: *mut NscFrame) -> bool {
    guard(false, || {
        let (Some(req), Some(out)) = (unsafe { req.as_ref() }, unsafe { out.as_mut() }) else {
            return false;
        };
        *out = NscFrame::empty();

        let mut guard = state();
        let Some(reg) = guard.as_mut() else {
            return false;
        };
        let play = reg.settings().default_play().with_rule(
            unsafe { opt_str(req.rule_path) },
            unsafe { opt_str(req.rule_speed) },
            unsafe { opt_str(req.rule_loop) },
        );
        let Some(view) = reg.resolve(&play, Instant::now()) else {
            return false;
        };

        out.cover_id = view.id;
        out.generation = view.frame.generation;
        match &view.frame.data {
            FrameData::Cpu(f) => {
                out.kind = NSC_FRAME_CPU;
                out.width = f.width;
                out.height = f.height;
                out.stride = f.stride;
                out.pixels = f.pixels.as_ptr();
                out.fourcc = DRM_FORMAT_ARGB8888;
            }
            #[cfg(target_os = "linux")]
            FrameData::DmaBuf(d) => {
                use std::os::fd::AsRawFd;
                if d.planes.len() > out.planes.len() {
                    return false;
                }
                out.kind = NSC_FRAME_DMABUF;
                out.width = d.width;
                out.height = d.height;
                out.fourcc = d.fourcc;
                out.modifier = d.modifier;
                out.plane_count = d.planes.len() as u32;
                for (dst, p) in out.planes.iter_mut().zip(&d.planes) {
                    *dst = NscPlane {
                        fd: p.fd.as_raw_fd(),
                        offset: p.offset,
                        stride: p.stride,
                    };
                }
            }
        }
        true
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_end_frame() {
    guard((), || {
        if let Some(reg) = state().as_mut() {
            reg.end_frame(Instant::now());
        }
    })
}

/// Идёт ли анимация (видео/GIF) на обложках, показанных за последнюю секунду.
#[unsafe(no_mangle)]
pub extern "C" fn nsc_animating() -> bool {
    guard(false, || {
        state()
            .as_ref()
            .is_some_and(|r| r.animating(Instant::now()))
    })
}

/// Жива ли ещё обложка (прослойка чистит кэш текстур по мёртвым id).
#[unsafe(no_mangle)]
pub extern "C" fn nsc_cover_alive(id: u64) -> bool {
    guard(false, || {
        state()
            .as_ref()
            .is_some_and(|r| r.live_ids().any(|i| i == id))
    })
}

/// Забрать одно уведомление для пользователя. Возвращает длину без NUL;
/// 0 — сообщений нет. Длинное сообщение обрезается по границе символа.
///
/// # Safety
/// `buf` указывает на `cap` байт.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_take_notification(buf: *mut c_char, cap: usize) -> usize {
    guard(0, || {
        if buf.is_null() || cap == 0 {
            return 0;
        }
        let Some(msg) = state().as_mut().and_then(|r| r.notifier().pop()) else {
            return 0;
        };
        let mut n = msg.len().min(cap - 1);
        while !msg.is_char_boundary(n) {
            n -= 1;
        }
        unsafe {
            std::ptr::copy_nonoverlapping(msg.as_ptr(), buf.cast::<u8>(), n);
            *buf.add(n) = 0;
        }
        n
    })
}

/// Extra rects одного монитора. Возвращает общее число (может быть больше `cap`).
///
/// # Safety
/// `out` указывает на `cap` элементов (или NULL при `cap == 0`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_extra_rects(
    monitor: i64,
    out: *mut NscExtraRect,
    cap: usize,
) -> usize {
    guard(0, || {
        let rects = extra::for_monitor(monitor);
        if !out.is_null() {
            for (i, r) in rects.iter().take(cap).enumerate() {
                unsafe {
                    *out.add(i) = NscExtraRect {
                        x: r.x,
                        y: r.y,
                        w: r.w,
                        h: r.h,
                        rounding: r.rounding,
                        window: r.window,
                        fill: r.fill as u32,
                    }
                };
            }
        }
        rects.len()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;
    use std::mem::{offset_of, size_of};

    /// Те же числа, что в static_assert заголовка include/noshare_cover.h.
    #[test]
    #[cfg(target_pointer_width = "64")]
    fn abi_layout() {
        assert_eq!(size_of::<NscFrame>(), 112);
        assert_eq!(offset_of!(NscFrame, pixels), 32);
        assert_eq!(offset_of!(NscFrame, modifier), 48);
        assert_eq!(offset_of!(NscFrame, planes), 60);
        assert_eq!(size_of::<NscPlane>(), 12);
        assert_eq!(size_of::<NscSettings>(), 40);
        assert_eq!(offset_of!(NscSettings, speed), 16);
        assert_eq!(size_of::<NscPlayRequest>(), 24);
        assert_eq!(size_of::<NscExtraRect>(), 56);
        assert_eq!(offset_of!(NscExtraRect, fill), 48);
        assert_eq!(size_of::<crate::extra::CRect>(), 56);
        assert_eq!(offset_of!(crate::extra::CRect, window), 40);
    }

    // Всё через глобальное состояние — один сценарий целиком.
    #[test]
    fn full_lifecycle_through_c_abi() {
        let dir = std::env::temp_dir().join(format!("nsc-ffi-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let png = dir.join("c.png");
        image::RgbaImage::from_pixel(4, 3, image::Rgba([0, 255, 0, 255]))
            .save(&png)
            .unwrap();

        assert!(nsc_init());
        let path = CString::new(png.to_str().unwrap()).unwrap();
        let backend = CString::new("gpu").unwrap();
        let settings = NscSettings {
            path_cover: path.as_ptr(),
            looped: true,
            speed: 1.0,
            backend: backend.as_ptr(),
            gpu_device: std::ptr::null(),
        };
        unsafe { nsc_set_settings(&settings) };
        let epoch = nsc_epoch();
        assert!(epoch > 0);

        let req = NscPlayRequest {
            rule_path: std::ptr::null(),
            rule_speed: std::ptr::null(),
            rule_loop: std::ptr::null(),
        };
        let mut out = NscFrame::empty();
        nsc_begin_frame();
        assert!(unsafe { nsc_resolve(&req, &mut out) });
        assert_eq!(
            (out.kind, out.width, out.height, out.stride),
            (NSC_FRAME_CPU, 4, 3, 16)
        );
        let px = unsafe { std::slice::from_raw_parts(out.pixels, 4) };
        assert_eq!(px, &[0, 255, 0, 255]);
        assert!(nsc_cover_alive(out.cover_id));
        nsc_end_frame();

        // правило окна с несуществующим файлом -> нечего рисовать + уведомление
        let missing = CString::new("/definitely/missing.gif").unwrap();
        let req2 = NscPlayRequest {
            rule_path: missing.as_ptr(),
            rule_speed: std::ptr::null(),
            rule_loop: std::ptr::null(),
        };
        nsc_begin_frame();
        assert!(!unsafe { nsc_resolve(&req2, &mut out) });
        nsc_end_frame();
        let mut buf = [0 as c_char; 256];
        let n = unsafe { nsc_take_notification(buf.as_mut_ptr(), buf.len()) };
        assert!(n > 0);
        let msg = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_str().unwrap();
        assert!(msg.contains("нет файла"), "{msg}");

        // тот же конфиг — эпоха не меняется
        unsafe { nsc_set_settings(&settings) };
        assert_eq!(nsc_epoch(), epoch);

        // tiny buffer: обрезка по границе UTF-8 без паники
        let long = CString::new("/нет/такого/файла/с/длинным/путём.gif").unwrap();
        let req3 = NscPlayRequest {
            rule_path: long.as_ptr(),
            rule_speed: std::ptr::null(),
            rule_loop: std::ptr::null(),
        };
        nsc_begin_frame();
        unsafe { nsc_resolve(&req3, &mut out) };
        let mut small = [0 as c_char; 8];
        let n = unsafe { nsc_take_notification(small.as_mut_ptr(), small.len()) };
        assert!(n < 8);
        assert!(unsafe { CStr::from_ptr(small.as_ptr()) }.to_str().is_ok());

        nsc_shutdown();
        assert_eq!(nsc_epoch(), 0);
        assert!(
            !unsafe { nsc_resolve(&req, &mut out) },
            "after shutdown nothing resolves"
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
