//! H.264 на CPU: системный libopenh264 через dlopen.
//!
//! Библиотеку не вшиваем (патенты H.264: Cisco платит за свои сборки, а
//! дистрибутивы кладут её отдельным пакетом — `openh264` в Arch, `openh264` в
//! nixpkgs). Нет библиотеки — понятная ошибка, плагин живёт дальше.
//!
//! Метки времени отдаём самому openh264 (uiInBsTimeStamp → uiOutYuvTimeStamp):
//! с B-кадрами он выдаёт картинки в порядке показа, и pts едет вместе с кадром.
//! Обёртка крейта этого не умеет, поэтому декодируем через его raw API.

use std::ptr::null_mut;
use std::time::Duration;

use openh264::OpenH264API;
use openh264::decoder::{Decoder as Oh264, DecoderConfig};
use openh264_sys2::{
    DECODER_OPTION, DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, SBufferInfo,
};

use crate::frame::FrameData;
use crate::media::video::pipeline::{DecodedFrame, Decoder, Packet, PipeResult};
use crate::media::video::yuv::{self, Chroma, ChromaPlanes, Matrix, Plane, YuvImage};

/// Где искать библиотеку: сперва по soname (ld.so сам найдёт в /usr/lib,
/// /run/opengl-driver, NixOS-овском LD_LIBRARY_PATH), потом по полным путям.
const CANDIDATES: &[&str] = &[
    "libopenh264.so.8",
    "libopenh264.so.7",
    "libopenh264.so",
    "/usr/lib/libopenh264.so",
    "/usr/lib64/libopenh264.so",
    "/usr/lib/x86_64-linux-gnu/libopenh264.so",
];

/// Переменная окружения для явного пути (например, скачанная сборка Cisco).
const ENV_PATH: &str = "NOSHARE_COVER_OPENH264";

pub struct H264Decoder {
    dec: Oh264,
}

fn load_api() -> Result<OpenH264API, String> {
    let mut tried = Vec::new();
    let explicit = std::env::var(ENV_PATH).ok();
    // NSC_LIB_OPENH264 — абсолютный путь, вшитый при сборке (Nix: библиотека из store,
    // на NixOS dlopen по soname её не найдёт)
    for name in explicit
        .iter()
        .map(String::as_str)
        .chain(option_env!("NSC_LIB_OPENH264"))
        .chain(CANDIDATES.iter().copied())
    {
        // SAFETY: openh264 держит стабильный C ABI для декодера в 2.x (so.6–so.8);
        // проверка хэша крейта пропускает только сборки Cisco, дистрибутивные
        // собраны из тех же исходников.
        match unsafe { OpenH264API::from_blob_path_unchecked(name) } {
            Ok(api) => return Ok(api),
            Err(e) => tried.push(format!("{name}: {e}")),
        }
    }
    Err(format!(
        "libopenh264 не найден (поставьте пакет openh264 или задайте {ENV_PATH}); пробовал: {}",
        tried.join("; ")
    ))
}

impl H264Decoder {
    pub fn new() -> PipeResult<Self> {
        let api = load_api()?;
        let dec = Oh264::with_api_config(api, DecoderConfig::new())
            .map_err(|e| format!("openh264: {e}"))?;
        Ok(Self { dec })
    }

    fn remaining(&mut self) -> usize {
        let mut n: DECODER_OPTION = 0;
        // SAFETY: GetOption пишет одно целое по указателю.
        let r = unsafe {
            self.dec.raw_api().get_option(
                DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER,
                (&raw mut n).cast(),
            )
        };
        if r == 0 {
            usize::try_from(n).unwrap_or(0)
        } else {
            0
        }
    }
}

/// Кадр из буфера openh264. Плоскости живут до следующего вызова декодера,
/// поэтому переводим в BGRA сразу.
fn convert(dst: &[*mut u8; 3], info: &SBufferInfo) -> Option<DecodedFrame> {
    if info.iBufferStatus != 1 || dst.iter().any(|p| p.is_null()) {
        return None;
    }
    // SAFETY: при iBufferStatus == 1 в объединении лежит sSystemBuffer.
    let sb = unsafe { info.UsrData.sSystemBuffer };
    let (w, h) = (
        usize::try_from(sb.iWidth).ok()?,
        usize::try_from(sb.iHeight).ok()?,
    );
    let (ys, cs) = (
        usize::try_from(sb.iStride[0]).ok()?,
        usize::try_from(sb.iStride[1]).ok()?,
    );
    if w == 0 || h == 0 {
        return None;
    }
    let (cw, ch) = (w.div_ceil(2), h.div_ceil(2));
    // SAFETY: openh264 гарантирует stride*строк байт на плоскость (I420).
    let plane = |p: *mut u8, stride: usize, width: usize, rows: usize| Plane {
        data: unsafe { std::slice::from_raw_parts(p, stride * (rows - 1) + width) },
        stride,
    };
    // openh264 отдаёт только 8-битный 4:2:0; матрицу VUI не показывает.
    let yuv = YuvImage {
        width: u32::try_from(w).ok()?,
        height: u32::try_from(h).ok()?,
        bit_depth: 8,
        chroma: Chroma::Sub420,
        matrix: Matrix::guess(u32::try_from(h).ok()?),
        full_range: false,
        y: plane(dst[0], ys, w, h),
        uv: ChromaPlanes::Planar {
            u: plane(dst[1], cs, cw, ch),
            v: plane(dst[2], cs, cw, ch),
        },
    };
    Some(DecodedFrame {
        pts: Duration::from_micros(info.uiOutYuvTimeStamp),
        data: FrameData::Cpu(yuv::to_bgra(&yuv)?),
    })
}

impl Decoder for H264Decoder {
    fn name(&self) -> &'static str {
        "openh264 (CPU)"
    }

    fn decode(&mut self, packet: &Packet) -> PipeResult<Vec<DecodedFrame>> {
        let Ok(len) = i32::try_from(packet.data.len()) else {
            return Ok(Vec::new());
        };
        let mut dst = [null_mut::<u8>(); 3];
        let mut info = SBufferInfo {
            uiInBsTimeStamp: u64::try_from(packet.pts.as_micros()).unwrap_or(u64::MAX),
            ..Default::default()
        };
        // SAFETY: пакет живёт весь вызов; dst/info — наши выходные буферы.
        // Код возврата не смотрим: битый кадр просто не даст картинки, а
        // следующий ключевой всё починит.
        unsafe {
            self.dec.raw_api().decode_frame_no_delay(
                packet.data.as_ptr(),
                len,
                dst.as_mut_ptr(),
                &raw mut info,
            );
        }
        Ok(convert(&dst, &info).into_iter().collect())
    }

    fn flush(&mut self) -> PipeResult<Vec<DecodedFrame>> {
        let mut out = Vec::new();
        for _ in 0..self.remaining() {
            let mut dst = [null_mut::<u8>(); 3];
            let mut info = SBufferInfo::default();
            // SAFETY: как в decode.
            unsafe {
                self.dec
                    .raw_api()
                    .flush_frame(dst.as_mut_ptr(), &raw mut info)
            };
            out.extend(convert(&dst, &info));
        }
        Ok(out)
    }

    fn reset(&mut self) {
        // После петли демуксер начинает с ключевого кадра с SPS/PPS, декодер
        // перестраивается сам; хвост прошлого круга просто выбрасываем.
        let _ = self.flush();
    }
}
