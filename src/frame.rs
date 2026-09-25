//! Готовый кадр: либо пиксели в памяти, либо dmabuf прямо из декодера GPU.
//!
//! Пиксели — premultiplied BGRA в памяти (= `DRM_FORMAT_ARGB8888` на little-endian),
//! то есть ровно то, что Hyprland принимает в `createTexture(drmFormat, pixels, …)`
//! без перепаковки. dmabuf уходит в `createTexture(SDMABUFAttrs)` — без копий через CPU.

use std::sync::Arc;

/// fourcc `AR24` = `DRM_FORMAT_ARGB8888`.
pub const DRM_FORMAT_ARGB8888: u32 = u32::from_le_bytes(*b"AR24");

#[derive(Debug, Clone)]
pub struct Frame {
    /// Растёт с каждым новым кадром источника. Прослойка обновляет текстуру,
    /// только когда номер сменился.
    pub generation: u64,
    pub data: FrameData,
}

#[derive(Debug, Clone)]
pub enum FrameData {
    Cpu(CpuFrame),
    #[cfg(target_os = "linux")]
    DmaBuf(Arc<DmaBufFrame>),
}

impl FrameData {
    pub fn size(&self) -> (u32, u32) {
        match self {
            Self::Cpu(f) => (f.width, f.height),
            #[cfg(target_os = "linux")]
            Self::DmaBuf(f) => (f.width, f.height),
        }
    }
}

/// Кадр в памяти. `Arc` — чтобы отдать указатель в C++ без копии и не бояться,
/// что декодер перезапишет буфер, пока текстура из него грузится.
#[derive(Debug, Clone)]
pub struct CpuFrame {
    pub width: u32,
    pub height: u32,
    /// Байт на строку.
    pub stride: u32,
    pub pixels: Arc<[u8]>,
}

impl CpuFrame {
    /// Из RGBA8 (как отдают декодеры картинок): premultiply + перестановка в BGRA.
    pub fn from_rgba(width: u32, height: u32, rgba: &[u8]) -> Self {
        debug_assert_eq!(rgba.len(), width as usize * height as usize * 4);
        let mut out = Vec::with_capacity(rgba.len());
        for &[r, g, b, a] in rgba.as_chunks::<4>().0 {
            out.extend_from_slice(&[premul(b, a), premul(g, a), premul(r, a), a]);
        }
        Self {
            width,
            height,
            stride: width * 4,
            pixels: out.into(),
        }
    }

    /// Уже готовый BGRA-буфер (например, холст GIF). Без копии.
    pub fn from_bgra(width: u32, height: u32, pixels: Arc<[u8]>) -> Self {
        debug_assert_eq!(pixels.len(), width as usize * height as usize * 4);
        Self {
            width,
            height,
            stride: width * 4,
            pixels,
        }
    }
}

/// `c * a / 255` с округлением, как в исходном плагине.
#[inline]
pub fn premul(c: u8, a: u8) -> u8 {
    ((u32::from(c) * u32::from(a) + 127) / 255) as u8
}

/// Кадр в видеопамяти, экспортированный декодером (`vaExportSurfaceHandle`).
/// Дескрипторы закрываются при drop — текстура Hyprland держит свою копию fd.
#[cfg(target_os = "linux")]
#[derive(Debug)]
pub struct DmaBufFrame {
    pub width: u32,
    pub height: u32,
    pub fourcc: u32,
    pub modifier: u64,
    pub planes: Vec<DmaBufPlane>,
}

#[cfg(target_os = "linux")]
#[derive(Debug)]
pub struct DmaBufPlane {
    pub fd: std::os::fd::OwnedFd,
    pub offset: u32,
    pub stride: u32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn premultiply_and_swizzle() {
        // полупрозрачный красный -> BGRA с премультиплаем
        let f = CpuFrame::from_rgba(1, 1, &[255, 0, 0, 128]);
        assert_eq!(&*f.pixels, &[0, 0, 128, 128]);
        assert_eq!(f.stride, 4);
    }

    #[test]
    fn opaque_is_untouched() {
        let f = CpuFrame::from_rgba(1, 1, &[10, 20, 30, 255]);
        assert_eq!(&*f.pixels, &[30, 20, 10, 255]);
    }

    #[test]
    fn fourcc_matches_drm() {
        assert_eq!(DRM_FORMAT_ARGB8888, 0x3432_5241);
    }
}
