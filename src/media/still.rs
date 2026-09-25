//! PNG и JPEG: декодируем один раз, дальше кадр не меняется.

use std::path::Path;
use std::time::Instant;

use super::{Kind, MediaError, Source};
use crate::frame::{CpuFrame, Frame, FrameData};

pub struct StillSource {
    frame: Option<Frame>,
}

impl StillSource {
    pub fn open(path: &Path) -> Result<Self, MediaError> {
        let img = image::ImageReader::open(path)
            .and_then(|r| r.with_guessed_format())
            .map_err(|e| open_err(path, e))?
            .decode()
            .map_err(|e| open_err(path, e))?
            .into_rgba8();
        let (w, h) = img.dimensions();
        if w == 0 || h == 0 {
            return Err(open_err(path, "пустая картинка"));
        }
        let frame = Frame {
            generation: 1,
            data: FrameData::Cpu(CpuFrame::from_rgba(w, h, img.as_raw())),
        };
        Ok(Self { frame: Some(frame) })
    }
}

impl Source for StillSource {
    fn kind(&self) -> Kind {
        Kind::Still
    }

    fn poll(&mut self, _now: Instant) -> Result<Option<Frame>, MediaError> {
        // отдаём кадр один раз: прослойка держит текстуру у себя
        Ok(self.frame.take())
    }
}

fn open_err(path: &Path, e: impl std::fmt::Display) -> MediaError {
    MediaError::Open {
        path: path.display().to_string(),
        reason: e.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn png_roundtrip_once() {
        let dir = std::env::temp_dir().join(format!("nsc-still-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("t.png");
        image::RgbaImage::from_pixel(3, 2, image::Rgba([255, 0, 0, 255]))
            .save(&p)
            .unwrap();

        let mut s = StillSource::open(&p).unwrap();
        let f = s
            .poll(Instant::now())
            .unwrap()
            .expect("first poll gives frame");
        assert_eq!(f.data.size(), (3, 2));
        assert!(
            s.poll(Instant::now()).unwrap().is_none(),
            "still image is sent once"
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn garbage_is_an_error_not_a_panic() {
        let dir = std::env::temp_dir().join(format!("nsc-still-bad-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("bad.png");
        std::fs::write(&p, b"not a png").unwrap();
        assert!(matches!(
            StillSource::open(&p),
            Err(MediaError::Open { .. })
        ));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
