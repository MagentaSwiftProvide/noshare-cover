//! VP8 и VP9 на CPU: системный libvpx через dlopen.
//!
//! Своя минимальная FFI по заголовкам libvpx (vpx_decoder.h, vpx_image.h):
//! шесть функций и две структуры. ABI декодера у libvpx не менялся с 1.8
//! (VPX_DECODER_ABI_VERSION = 12), soname при этом растёт — пробуем все.
//! pts едет через `user_priv`: VP8/VP9 не переставляют кадры, но скрытые
//! (altref) кадры картинки не дают, и метка остаётся за своим кадром.

use std::ffi::{CStr, c_char, c_int, c_long, c_uint, c_void};
use std::ptr::{null, null_mut};
use std::time::Duration;

use libloading::Library;

use crate::frame::FrameData;
use crate::media::video::pipeline::{Codec, DecodedFrame, Decoder, Packet, PipeResult};
use crate::media::video::yuv::{self, Chroma, ChromaPlanes, Matrix, Plane, YuvImage};

const CANDIDATES: &[&str] = &[
    "libvpx.so.11",
    "libvpx.so.10",
    "libvpx.so.9",
    "libvpx.so.8",
    "libvpx.so.7",
    "libvpx.so.6",
    "libvpx.so",
];
const ENV_PATH: &str = "NOSHARE_COVER_LIBVPX";

const VPX_IMAGE_ABI_VERSION: c_int = 5;
const VPX_CODEC_ABI_VERSION: c_int = 4 + VPX_IMAGE_ABI_VERSION;
const VPX_DECODER_ABI_VERSION: c_int = 3 + VPX_CODEC_ABI_VERSION;

const FMT_PLANAR: c_int = 0x100;
const FMT_UV_FLIP: c_int = 0x200;
const FMT_HIGHBITDEPTH: c_int = 0x800;
const FMT_I420: c_int = FMT_PLANAR | 2;
const FMT_YV12: c_int = FMT_PLANAR | FMT_UV_FLIP | 1;
const FMT_I422: c_int = FMT_PLANAR | 5;
const FMT_I444: c_int = FMT_PLANAR | 6;

const CS_BT_709: c_int = 2;
const CS_BT_2020: c_int = 5;

#[repr(C)]
struct VpxCodecCtx {
    name: *const c_char,
    iface: *const c_void,
    err: c_int,
    err_detail: *const c_char,
    init_flags: c_long,
    config: *const c_void,
    priv_: *mut c_void,
}

#[repr(C)]
struct VpxDecCfg {
    threads: c_uint,
    w: c_uint,
    h: c_uint,
}

#[repr(C)]
struct VpxImage {
    fmt: c_int,
    cs: c_int,
    range: c_int,
    w: c_uint,
    h: c_uint,
    bit_depth: c_uint,
    d_w: c_uint,
    d_h: c_uint,
    r_w: c_uint,
    r_h: c_uint,
    x_chroma_shift: c_uint,
    y_chroma_shift: c_uint,
    planes: [*mut u8; 4],
    stride: [c_int; 4],
    bps: c_int,
    user_priv: *mut c_void,
    img_data: *mut u8,
    img_data_owner: c_int,
    self_allocd: c_int,
    fb_priv: *mut c_void,
}

type FnIface = unsafe extern "C" fn() -> *const c_void;
type FnInit =
    unsafe extern "C" fn(*mut VpxCodecCtx, *const c_void, *const VpxDecCfg, c_long, c_int) -> c_int;
type FnDecode =
    unsafe extern "C" fn(*mut VpxCodecCtx, *const u8, c_uint, *mut c_void, c_long) -> c_int;
type FnGetFrame = unsafe extern "C" fn(*mut VpxCodecCtx, *mut *const c_void) -> *const VpxImage;
type FnDestroy = unsafe extern "C" fn(*mut VpxCodecCtx) -> c_int;
type FnErrStr = unsafe extern "C" fn(c_int) -> *const c_char;

struct Api {
    decode: FnDecode,
    get_frame: FnGetFrame,
    destroy: FnDestroy,
    err_str: FnErrStr,
    // держим библиотеку, пока живут указатели на её функции
    _lib: Library,
}

pub struct VpxDecoder {
    api: Api,
    // Box: libvpx хранит указатель на контекст внутри себя, адрес не должен меняться
    ctx: Box<VpxCodecCtx>,
    vp9: bool,
}

// Контекст используется только из потока декодера.
unsafe impl Send for VpxDecoder {}

fn open_lib() -> Result<Library, String> {
    let mut tried = Vec::new();
    let explicit = std::env::var(ENV_PATH).ok();
    // NSC_LIB_VPX — абсолютный путь, вшитый при сборке (Nix: библиотека из store,
    // на NixOS dlopen по soname её не найдёт)
    for name in explicit
        .iter()
        .map(String::as_str)
        .chain(option_env!("NSC_LIB_VPX"))
        .chain(CANDIDATES.iter().copied())
    {
        // SAFETY: у libvpx нет конструкторов с побочными эффектами.
        match unsafe { Library::new(name) } {
            Ok(l) => return Ok(l),
            Err(e) => tried.push(format!("{name}: {e}")),
        }
    }
    Err(format!(
        "libvpx не найден (поставьте пакет libvpx или задайте {ENV_PATH}); пробовал: {}",
        tried.join("; ")
    ))
}

impl VpxDecoder {
    pub fn new(codec: &Codec) -> PipeResult<Self> {
        let vp9 = match codec {
            Codec::Vp8 => false,
            Codec::Vp9 => true,
            other => return Err(format!("libvpx не декодирует {other:?}")),
        };
        let lib = open_lib()?;
        // SAFETY: сигнатуры — из vpx_decoder.h / vp8dx.h.
        let (iface, init, api) = unsafe {
            let sym = |n: &[u8]| -> Result<*const c_void, String> {
                lib.get::<*const c_void>(n).map(|s| *s).map_err(|e| {
                    format!(
                        "libvpx: нет {}: {e}",
                        String::from_utf8_lossy(&n[..n.len() - 1])
                    )
                })
            };
            let iface_fn: FnIface = std::mem::transmute(sym(if vp9 {
                b"vpx_codec_vp9_dx\0"
            } else {
                b"vpx_codec_vp8_dx\0"
            })?);
            let init: FnInit = std::mem::transmute(sym(b"vpx_codec_dec_init_ver\0")?);
            let api = Api {
                decode: std::mem::transmute::<*const c_void, FnDecode>(sym(b"vpx_codec_decode\0")?),
                get_frame: std::mem::transmute::<*const c_void, FnGetFrame>(sym(
                    b"vpx_codec_get_frame\0",
                )?),
                destroy: std::mem::transmute::<*const c_void, FnDestroy>(sym(
                    b"vpx_codec_destroy\0",
                )?),
                err_str: std::mem::transmute::<*const c_void, FnErrStr>(sym(
                    b"vpx_codec_err_to_string\0",
                )?),
                _lib: lib,
            };
            (iface_fn(), init, api)
        };
        if iface.is_null() {
            return Err("libvpx собран без декодера для этого кодека".into());
        }
        let mut ctx = Box::new(VpxCodecCtx {
            name: null(),
            iface: null(),
            err: 0,
            err_detail: null(),
            init_flags: 0,
            config: null(),
            priv_: null_mut(),
        });
        let cfg = VpxDecCfg {
            threads: std::thread::available_parallelism()
                .map_or(2, |n| n.get().clamp(1, 4) as c_uint),
            w: 0,
            h: 0,
        };
        // SAFETY: ctx — наш буфер нужного размера, cfg живёт весь вызов.
        let r = unsafe {
            init(
                &raw mut *ctx,
                iface,
                &raw const cfg,
                0,
                VPX_DECODER_ABI_VERSION,
            )
        };
        if r != 0 {
            return Err(format!("vpx_codec_dec_init: {}", err_text(&api, r)));
        }
        Ok(Self { api, ctx, vp9 })
    }

    fn drain(&mut self, out: &mut Vec<DecodedFrame>) {
        let mut iter: *const c_void = null();
        loop {
            // SAFETY: контекст инициализирован; картинка живёт до следующего decode.
            let img = unsafe { (self.api.get_frame)(&raw mut *self.ctx, &raw mut iter) };
            let Some(img) = (unsafe { img.as_ref() }) else {
                break;
            };
            out.extend(convert(img));
        }
    }
}

fn err_text(api: &Api, code: c_int) -> String {
    // SAFETY: err_to_string возвращает статическую строку.
    let p = unsafe { (api.err_str)(code) };
    if p.is_null() {
        return format!("код {code}");
    }
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

fn convert(img: &VpxImage) -> Option<DecodedFrame> {
    let hbd = img.fmt & FMT_HIGHBITDEPTH != 0;
    let (chroma, flip) = match img.fmt & !FMT_HIGHBITDEPTH {
        FMT_I420 => (Chroma::Sub420, false),
        FMT_YV12 => (Chroma::Sub420, true),
        FMT_I422 => (Chroma::Sub422, false),
        FMT_I444 => (Chroma::Full, false),
        _ => return None,
    };
    let (w, h) = (img.d_w as usize, img.d_h as usize);
    if w == 0 || h == 0 || img.planes[..3].iter().any(|p| p.is_null()) {
        return None;
    }
    let bit_depth = if hbd { img.bit_depth.max(9) } else { 8 };
    let bpp = if hbd { 2 } else { 1 };
    let (sx, sy) = (img.x_chroma_shift, img.y_chroma_shift);
    let (cw, ch) = (w.div_ceil(1 << sx), h.div_ceil(1 << sy));
    let plane = |i: usize, width: usize, rows: usize| -> Option<Plane<'_>> {
        let stride = usize::try_from(img.stride[i]).ok()?;
        // SAFETY: libvpx выделяет stride*строк байт на плоскость.
        Some(Plane {
            data: unsafe {
                std::slice::from_raw_parts(img.planes[i], stride * (rows - 1) + width * bpp)
            },
            stride,
        })
    };
    let (ui, vi) = if flip { (2, 1) } else { (1, 2) };
    let matrix = match img.cs {
        CS_BT_709 => Matrix::Bt709,
        CS_BT_2020 => Matrix::Bt2020,
        0 => Matrix::guess(img.d_h),
        _ => Matrix::Bt601,
    };
    let yuv = YuvImage {
        width: img.d_w,
        height: img.d_h,
        bit_depth,
        chroma,
        matrix,
        full_range: img.range == 1,
        y: plane(0, w, h)?,
        uv: ChromaPlanes::Planar {
            u: plane(ui, cw, ch)?,
            v: plane(vi, cw, ch)?,
        },
    };
    Some(DecodedFrame {
        pts: Duration::from_micros(img.user_priv as u64),
        data: FrameData::Cpu(yuv::to_bgra(&yuv)?),
    })
}

impl Decoder for VpxDecoder {
    fn name(&self) -> &'static str {
        if self.vp9 {
            "libvpx VP9 (CPU)"
        } else {
            "libvpx VP8 (CPU)"
        }
    }

    fn decode(&mut self, packet: &Packet) -> PipeResult<Vec<DecodedFrame>> {
        let mut out = Vec::new();
        let Ok(len) = c_uint::try_from(packet.data.len()) else {
            return Ok(out);
        };
        // pts в микросекундах прямо в указателе: libvpx его не разыменовывает
        let tag = usize::try_from(packet.pts.as_micros()).unwrap_or(usize::MAX) as *mut c_void;
        // SAFETY: пакет живёт весь вызов; deadline 0 = лучшее качество.
        let r = unsafe { (self.api.decode)(&raw mut *self.ctx, packet.data.as_ptr(), len, tag, 0) };
        if r == 0 {
            self.drain(&mut out);
        }
        // битый кадр пропускаем: следующий ключевой всё починит
        Ok(out)
    }

    fn flush(&mut self) -> PipeResult<Vec<DecodedFrame>> {
        let mut out = Vec::new();
        // NULL-данные = конец потока, декодер отдаёт что держал
        // SAFETY: так flush описан в vpx_decoder.h.
        let r = unsafe { (self.api.decode)(&raw mut *self.ctx, null(), 0, null_mut(), 0) };
        if r == 0 {
            self.drain(&mut out);
        }
        Ok(out)
    }

    fn reset(&mut self) {
        // VP8/VP9 начинают с ключевого кадра, который сбрасывает все ссылки.
        let _ = self.flush();
    }
}

impl Drop for VpxDecoder {
    fn drop(&mut self) {
        // SAFETY: контекст инициализирован в new и уничтожается один раз.
        unsafe { (self.api.destroy)(&raw mut *self.ctx) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn struct_layouts_match_libvpx_on_lp64() {
        if cfg!(target_pointer_width = "64") {
            assert_eq!(std::mem::size_of::<VpxCodecCtx>(), 56);
        }
        assert_eq!(std::mem::offset_of!(VpxImage, planes), 48);
        assert_eq!(std::mem::offset_of!(VpxImage, stride), 80);
        assert_eq!(std::mem::offset_of!(VpxImage, user_priv), 104);
    }
}
