//! VA-API-декодер для noshare-cover: тонкая C-обёртка над stateless-декодерами
//! cros-codecs. Грузится основным плагином через dlopen (из memfd), поэтому
//! весь интерфейс — несколько `extern "C"` функций и одна структура кадра.
//!
//! Кадры выводятся в GBM-буферы NV12, после декода отображаются в память
//! (gbm_bo_map сам снимает тайлинг) и отдаются колбэком без копий: указатели
//! живут только на время вызова колбэка.
//!
//! Ограничение cros-codecs 0.0.6: VA-API путь только 8-битный (NV12).

#![cfg_attr(not(target_os = "linux"), allow(unused))]

use std::ffi::{c_char, c_void};

/// Версия ABI между помощником и плагином.
pub const ABI: u32 = 1;

#[repr(C)]
pub struct NscVaFrame {
    pub width: u32,
    pub height: u32,
    pub y: *const u8,
    pub y_stride: usize,
    pub uv: *const u8,
    pub uv_stride: usize,
    pub pts_ns: u64,
}

pub type FrameCb = unsafe extern "C" fn(user: *mut c_void, frame: *const NscVaFrame);

// Коды кодеков — совпадают с decode/vaapi.rs основного плагина.
pub const CODEC_H264: u32 = 1;
pub const CODEC_HEVC: u32 = 2;
pub const CODEC_VP8: u32 = 3;
pub const CODEC_VP9: u32 = 4;
pub const CODEC_AV1: u32 = 5;

#[unsafe(no_mangle)]
pub extern "C" fn nsc_vaapi_abi() -> u32 {
    ABI
}

fn write_err(buf: *mut c_char, len: usize, msg: &str) {
    if buf.is_null() || len == 0 {
        return;
    }
    let bytes = msg.as_bytes();
    let n = bytes.len().min(len - 1);
    // SAFETY: вызывающий дал буфер на len байт.
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf.cast::<u8>(), n);
        *buf.add(n) = 0;
    }
}

/// Паника не должна пересечь C-границу.
fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or(fallback)
}

#[cfg(target_os = "linux")]
mod imp {
    use super::*;
    use std::ffi::CStr;
    use std::rc::Rc;
    use std::sync::Arc;

    use cros_codecs::decoder::stateless::av1::Av1;
    use cros_codecs::decoder::stateless::h264::H264;
    use cros_codecs::decoder::stateless::h265::H265;
    use cros_codecs::decoder::stateless::vp8::Vp8;
    use cros_codecs::decoder::stateless::vp9::Vp9;
    use cros_codecs::decoder::stateless::{DecodeError, DynStatelessVideoDecoder, StatelessDecoder, StatelessVideoDecoder};
    use cros_codecs::decoder::{DecodedHandle, DecoderEvent};
    use cros_codecs::libva::Display;
    use cros_codecs::video_frame::VideoFrame;
    use cros_codecs::video_frame::frame_pool::{FramePool, PooledVideoFrame};
    use cros_codecs::video_frame::gbm_video_frame::{GbmDevice, GbmUsage, GbmVideoFrame};
    use cros_codecs::{BlockingMode, Fourcc};

    type Frame = PooledVideoFrame<GbmVideoFrame>;

    pub struct Dec {
        // порядок полей = порядок drop: декодер отпускает кадры и контекст раньше пула и дисплея
        dec: DynStatelessVideoDecoder<Frame>,
        pool: FramePool<GbmVideoFrame>,
        _display: Rc<Display>,
        pub err: String,
    }

    /// Сколько кадров сверх минимума держать в пуле: запас, чтобы декодер не
    /// упирался в NotEnoughOutputBuffers, пока мы копируем кадр.
    const POOL_HEADROOM: usize = 2;

    pub fn open(node: &str, codec: u32) -> Result<Dec, String> {
        let display = Display::open_drm_display(node).map_err(|e| format!("VA-API на {node}: {e}"))?;
        let gbm = GbmDevice::open(node).map_err(|e| format!("GBM на {node}: {e}"))?;
        let pool = FramePool::new(move |si| {
            Arc::clone(&gbm)
                .new_frame(Fourcc::from(b"NV12"), si.display_resolution, si.coded_resolution, GbmUsage::Decode)
                .expect("GBM: не удалось выделить кадр NV12")
        });
        let bm = BlockingMode::Blocking;
        let d = Rc::clone(&display);
        let dec: DynStatelessVideoDecoder<Frame> = match codec {
            CODEC_H264 => StatelessDecoder::<H264, _>::new_vaapi(d, bm).map(|x| x.into_trait_object()),
            CODEC_HEVC => StatelessDecoder::<H265, _>::new_vaapi(d, bm).map(|x| x.into_trait_object()),
            CODEC_VP8 => StatelessDecoder::<Vp8, _>::new_vaapi(d, bm).map(|x| x.into_trait_object()),
            CODEC_VP9 => StatelessDecoder::<Vp9, _>::new_vaapi(d, bm).map(|x| x.into_trait_object()),
            CODEC_AV1 => StatelessDecoder::<Av1, _>::new_vaapi(d, bm).map(|x| x.into_trait_object()),
            other => return Err(format!("неизвестный кодек {other}")),
        }
        .map_err(|e| format!("VA-API не умеет этот кодек на {node}: {e}"))?;
        Ok(Dec { dec, pool, _display: display, err: String::new() })
    }

    impl Dec {
        /// Разобрать события декодера. `true` — что-то сдвинулось.
        fn drain(&mut self, cb: FrameCb, user: *mut c_void) -> Result<bool, String> {
            let mut progress = false;
            while let Some(ev) = self.dec.next_event() {
                progress = true;
                match ev {
                    DecoderEvent::FormatChanged => {
                        let mut si = self.dec.stream_info().ok_or("формат без stream_info")?.clone();
                        si.min_num_frames += POOL_HEADROOM;
                        self.pool.resize(&si);
                    }
                    DecoderEvent::FrameReady(h) => {
                        h.sync().map_err(|e| format!("синхронизация кадра: {e}"))?;
                        let pts_ns = h.timestamp();
                        let vf = h.video_frame();
                        let res = vf.resolution();
                        let pitch = vf.get_plane_pitch();
                        let map = vf.map().map_err(|e| format!("отображение кадра: {e}"))?;
                        let planes = map.get();
                        if planes.len() < 2 || pitch.len() < 2 {
                            return Err("кадр без двух плоскостей NV12".into());
                        }
                        let f = NscVaFrame {
                            width: res.width,
                            height: res.height,
                            y: planes[0].as_ptr(),
                            y_stride: pitch[0],
                            uv: planes[1].as_ptr(),
                            uv_stride: pitch[1],
                            pts_ns,
                        };
                        // SAFETY: колбэк плагина копирует данные внутри вызова.
                        unsafe { cb(user, &f) };
                    }
                }
            }
            Ok(progress)
        }

        pub fn decode(&mut self, data: &[u8], pts_ns: u64, cb: FrameCb, user: *mut c_void) -> Result<(), String> {
            let mut off = 0;
            let mut stalls = 0;
            while off < data.len() {
                let pool = &mut self.pool;
                match self.dec.decode(pts_ns, &data[off..], &mut || pool.alloc()) {
                    Ok(0) => break, // декодер ничего не взял — не крутимся вечно
                    Ok(n) => {
                        off += n;
                        stalls = 0;
                    }
                    Err(DecodeError::CheckEvents | DecodeError::NotEnoughOutputBuffers(_)) => {
                        if !self.drain(cb, user)? {
                            stalls += 1;
                            if stalls > 3 {
                                return Err("VA-API: декодеру не хватает кадров".into());
                            }
                        }
                    }
                    Err(e) => return Err(format!("VA-API: {e}")),
                }
            }
            self.drain(cb, user).map(|_| ())
        }

        pub fn flush(&mut self, cb: FrameCb, user: *mut c_void) -> Result<(), String> {
            self.dec.flush().map_err(|e| format!("VA-API flush: {e}"))?;
            self.drain(cb, user).map(|_| ())
        }

        pub fn reset(&mut self) {
            // после flush декодер ждёт ключевой кадр — ровно то, с чего начнётся новый круг
            let _ = self.dec.flush();
            while self.dec.next_event().is_some() {}
        }
    }

    pub fn cstr(p: *const c_char) -> Option<String> {
        if p.is_null() {
            return None;
        }
        // SAFETY: вызывающий передаёт C-строку.
        Some(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
    }

    pub fn with_dec(h: *mut c_void, f: impl FnOnce(&mut Dec) -> Result<(), String>) -> i32 {
        guard(-2, || {
            // SAFETY: h получен из nsc_vaapi_open и ещё не закрыт.
            let Some(d) = (unsafe { h.cast::<Dec>().as_mut() }) else { return -1 };
            match f(d) {
                Ok(()) => 0,
                Err(e) => {
                    d.err = e;
                    -1
                }
            }
        })
    }
}

/// Открыть декодер. NULL — ошибка, текст в `err`.
///
/// # Safety
/// `node` — C-строка, `err` — буфер на `err_len` байт или NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_open(node: *const c_char, codec: u32, err: *mut c_char, err_len: usize) -> *mut c_void {
    #[cfg(target_os = "linux")]
    {
        guard(std::ptr::null_mut(), || {
            let Some(node) = imp::cstr(node) else {
                write_err(err, err_len, "нет render node");
                return std::ptr::null_mut();
            };
            match imp::open(&node, codec) {
                Ok(d) => Box::into_raw(Box::new(d)).cast(),
                Err(e) => {
                    write_err(err, err_len, &e);
                    std::ptr::null_mut()
                }
            }
        })
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (node, codec);
        write_err(err, err_len, "VA-API есть только на Linux");
        std::ptr::null_mut()
    }
}

/// # Safety
/// `h` — от `nsc_vaapi_open`; `data` — `len` байт; `cb` зовётся синхронно.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_decode(h: *mut c_void, data: *const u8, len: usize, pts_ns: u64, cb: FrameCb, user: *mut c_void) -> i32 {
    #[cfg(target_os = "linux")]
    {
        if data.is_null() {
            return 0;
        }
        // SAFETY: см. контракт функции.
        let bytes = unsafe { std::slice::from_raw_parts(data, len) };
        imp::with_dec(h, |d| d.decode(bytes, pts_ns, cb, user))
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (h, data, len, pts_ns, cb, user);
        -1
    }
}

/// # Safety
/// Как у `nsc_vaapi_decode`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_flush(h: *mut c_void, cb: FrameCb, user: *mut c_void) -> i32 {
    #[cfg(target_os = "linux")]
    {
        imp::with_dec(h, |d| d.flush(cb, user))
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (h, cb, user);
        -1
    }
}

/// # Safety
/// `h` — от `nsc_vaapi_open`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_reset(h: *mut c_void) {
    #[cfg(target_os = "linux")]
    imp::with_dec(h, |d| {
        d.reset();
        Ok(())
    });
    #[cfg(not(target_os = "linux"))]
    let _ = h;
}

/// Текст последней ошибки. Возвращает длину без нуля.
///
/// # Safety
/// `h` — от `nsc_vaapi_open`, `buf` — `len` байт.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_error(h: *mut c_void, buf: *mut c_char, len: usize) -> usize {
    guard(0, || {
        // SAFETY: см. контракт.
        #[cfg(target_os = "linux")]
        if let Some(d) = unsafe { h.cast::<imp::Dec>().as_ref() } {
            write_err(buf, len, &d.err);
            return d.err.len();
        }
        let _ = (h, buf, len);
        0
    })
}

/// # Safety
/// `h` — от `nsc_vaapi_open`, закрывается один раз.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_vaapi_close(h: *mut c_void) {
    guard((), || {
        #[cfg(target_os = "linux")]
        if !h.is_null() {
            // SAFETY: Box из nsc_vaapi_open.
            drop(unsafe { Box::from_raw(h.cast::<imp::Dec>()) });
        }
        let _ = h;
    });
}
