//! Выбор демуксера и декодера под файл и настройки.
//!
//! Порядок: `backend = gpu` — только VA-API, ошибка если нельзя;
//! `cpu` — только программный; `auto` — VA-API, если GPU есть и умеет кодек,
//! иначе программный. Каждый отказ объясняется текстом, который уйдёт
//! пользователю, а не теряется молча.
//!
//! Статус: сами демуксеры (mp4/mov, webm/mkv) и декодеры (VA-API, dav1d, openh264)
//! подключаются следующим шагом — они требуют Linux-машины для проверки. Каркас,
//! часы, поток и жизненный цикл готовы и покрыты тестами (см. pipeline.rs, mod.rs).

use std::path::Path;

use super::pipeline::{Codec, Decoder, Demuxer, Pipeline};
use crate::config::{Backend, Settings};
use crate::gpu;

pub fn open_pipeline(path: &Path, looped: bool, settings: &Settings) -> Result<Pipeline, String> {
    let demux = open_demuxer(path)?;
    let decoder = open_decoder(&demux.info().codec, settings)?;
    Ok(Pipeline::new(demux, decoder, looped))
}

fn open_demuxer(path: &Path) -> Result<Box<dyn Demuxer>, String> {
    super::demux::open(path)
}

fn open_decoder(codec: &Codec, settings: &Settings) -> Result<Box<dyn Decoder>, String> {
    let mut reasons = Vec::new();

    if matches!(settings.backend, Backend::Auto | Backend::Gpu) {
        match gpu::pick_render_node(settings.gpu_device.as_deref()) {
            Ok(node) => match open_hw(codec, &node) {
                Ok(d) => return Ok(d),
                Err(e) => reasons.push(format!("GPU {}: {e}", node.display())),
            },
            Err(e) => reasons.push(format!("GPU: {e}")),
        }
        if settings.backend == Backend::Gpu {
            return Err(reasons.join("; "));
        }
    }

    match open_sw(codec) {
        Ok(d) => Ok(d),
        Err(e) => {
            reasons.push(format!("CPU: {e}"));
            Err(reasons.join("; "))
        }
    }
}

/// GPU-декодер для render node. NVIDIA — сначала NVDEC напрямую (драйвер
/// сам разбирает поток), потом VA-API (nvidia-vaapi-driver); Intel/AMD — VA-API.
fn open_hw(codec: &Codec, node: &Path) -> Result<Box<dyn Decoder>, String> {
    let mut reasons: Vec<String> = Vec::new();

    #[cfg(feature = "nvdec")]
    if super::decode::nvdec::node_is_nvidia(node) {
        match super::decode::nvdec::NvdecDecoder::new(codec, Some(node)) {
            Ok(d) => return Ok(Box::new(d)),
            Err(e) => reasons.push(format!("NVDEC: {e}")),
        }
    }

    #[cfg(all(feature = "vaapi", target_os = "linux"))]
    match super::decode::vaapi::VaapiDecoder::new(codec, node) {
        Ok(d) => return Ok(Box::new(d)),
        Err(e) => reasons.push(format!("VA-API: {e}")),
    }
    #[cfg(not(all(feature = "vaapi", target_os = "linux")))]
    reasons.push(format!(
        "VA-API недоступен в этой сборке ({codec:?}, {})",
        node.display()
    ));

    Err(reasons.join("; "))
}

fn open_sw(codec: &Codec) -> Result<Box<dyn Decoder>, String> {
    // Кадры YUV -> premultiplied BGRA одним проходом (yuv.rs).
    match codec {
        #[cfg(feature = "cpu-av1")]
        Codec::Av1 => Ok(Box::new(super::decode::av1::Av1Decoder::new()?)),
        #[cfg(feature = "cpu-h264")]
        Codec::H264 => Ok(Box::new(super::decode::h264::H264Decoder::new()?)),
        #[cfg(feature = "cpu-vpx")]
        Codec::Vp8 | Codec::Vp9 => Ok(Box::new(super::decode::vpx::VpxDecoder::new(codec)?)),
        _ => Err(format!(
            "программный декодер для {codec:?} ещё не подключён"
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gpu_only_reports_why() {
        let s = Settings {
            backend: Backend::Gpu,
            gpu_device: Some("/nonexistent/renderD999".into()),
            ..Settings::default()
        };
        let err = open_decoder(&Codec::Av1, &s).err().unwrap();
        assert!(err.contains("GPU"), "{err}");
        assert!(!err.contains("CPU"), "gpu-only must not fall back: {err}");
    }

    #[test]
    fn auto_tries_both() {
        let s = Settings {
            backend: Backend::Auto,
            gpu_device: Some("/nonexistent/renderD999".into()),
            ..Settings::default()
        };
        let err = open_decoder(&Codec::Other("V_TEST".into()), &s)
            .err()
            .unwrap();
        assert!(err.contains("GPU") && err.contains("CPU"), "{err}");
    }
}

#[cfg(test)]
mod real {
    use super::*;
    use crate::frame::FrameData;
    use crate::media::video::pipeline::Next;

    fn run_cpu(file: &str, decoder: &str) {
        let Some(dir) = std::env::var_os("NSC_TEST_MEDIA").map(std::path::PathBuf::from) else {
            return;
        };
        let path = dir.join(file);
        if !path.exists() {
            return;
        }
        let s = Settings {
            backend: Backend::Cpu,
            ..Settings::default()
        };
        let mut p = match open_pipeline(&path, true, &s) {
            Ok(p) => p,
            Err(e) if e.contains("не найден") => {
                eprintln!("{file}: {e} — пропуск");
                return;
            }
            Err(e) => panic!("{file}: {e}"),
        };
        assert_eq!(p.decoder_name(), decoder);
        let mut last = None;
        for n in 0..400 {
            let Next::Frame(f) = p.next_frame().unwrap() else {
                panic!("петля не должна кончаться")
            };
            #[allow(irrefutable_let_patterns)]
            let FrameData::Cpu(c) = &f.data else {
                panic!("ждали CPU-кадр")
            };
            assert_eq!((c.width, c.height), (640, 360));
            // NSC_DUMP=каталог — сохранить 60-й кадр в PNG, чтобы глазами проверить цвета
            if let (60, Some(d)) = (n, std::env::var_os("NSC_DUMP")) {
                let rgba: Vec<u8> = c
                    .pixels
                    .chunks(4)
                    .flat_map(|p| [p[2], p[1], p[0], p[3]])
                    .collect();
                image::save_buffer(
                    std::path::Path::new(&d).join(format!("{file}.png")),
                    &rgba,
                    c.width,
                    c.height,
                    image::ExtendedColorType::Rgba8,
                )
                .unwrap();
            }
            if let Some(prev) = last {
                assert!(
                    f.pts > prev,
                    "{file}: pts идут вперёд даже через петлю: {prev:?} -> {:?}",
                    f.pts
                );
            }
            last = Some(f.pts);
        }
        assert!(last.unwrap() > std::time::Duration::from_secs(10));
    }

    #[cfg(feature = "cpu-h264")]
    #[test]
    fn h264_mp4_decodes_on_cpu() {
        run_cpu("h264.mp4", "openh264 (CPU)");
    }

    #[cfg(feature = "cpu-vpx")]
    #[test]
    fn vp9_webm_decodes_on_cpu() {
        run_cpu("vp9.webm", "libvpx VP9 (CPU)");
    }

    #[cfg(feature = "cpu-av1")]
    #[test]
    fn av1_mp4_decodes_on_cpu() {
        run_cpu("av1.mp4", "rav1d (CPU)");
    }
}
