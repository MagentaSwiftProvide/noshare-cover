//! Источники медиа: картинка, GIF, видео. Все выглядят одинаково для реестра —
//! «дай кадр на такую позицию, если он поменялся».

pub mod gif;
pub mod still;
pub mod video;

use std::path::Path;
use std::time::{Duration, Instant};

use crate::config::{PlayParams, Settings};
use crate::frame::Frame;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Still,
    Gif,
    Video,
}

#[derive(Debug, thiserror::Error)]
pub enum MediaError {
    #[error("нет файла {0}")]
    Missing(String),
    #[error("не знаю формат {0}")]
    UnknownFormat(String),
    #[error("не открылся {path}: {reason}")]
    Open { path: String, reason: String },
    #[error("видео {path}: {reason}")]
    Video { path: String, reason: String },
}

/// Общий интерфейс источника.
pub trait Source: Send {
    fn kind(&self) -> Kind;

    /// Кадр, актуальный на `now`. `None` — картинка не поменялась с прошлого раза
    /// (прослойка просто рисует прежнюю текстуру). Для видео `now` ещё и сигнал
    /// «на меня смотрят»: без вызовов декодер засыпает.
    fn poll(&mut self, now: Instant) -> Result<Option<Frame>, MediaError>;
}

/// Расширения, которые понимаем. Регистр не важен.
pub fn kind_for(path: &Path) -> Option<Kind> {
    let ext = path.extension()?.to_str()?.to_ascii_lowercase();
    match ext.as_str() {
        "png" | "jpg" | "jpeg" => Some(Kind::Still),
        "gif" => Some(Kind::Gif),
        "mp4" | "m4v" | "mov" | "webm" | "mkv" => Some(Kind::Video),
        _ => None,
    }
}

/// Открыть источник по пути. Ошибка уже готова к показу пользователю.
pub fn open(play: &PlayParams, settings: &Settings) -> Result<Box<dyn Source>, MediaError> {
    let path = &play.path;
    let shown = path.display().to_string();
    if !path.exists() {
        return Err(MediaError::Missing(shown));
    }
    let kind = kind_for(path).ok_or_else(|| {
        MediaError::UnknownFormat(
            path.extension()
                .map(|e| format!(".{}", e.to_string_lossy()))
                .unwrap_or_else(|| shown.clone()),
        )
    })?;
    Ok(match kind {
        Kind::Still => Box::new(still::StillSource::open(path)?),
        Kind::Gif => Box::new(gif::GifSource::open(path, play.speed, play.looped)?),
        Kind::Video => Box::new(video::VideoSource::open(
            path,
            play.speed,
            play.looped,
            settings,
        )?),
    })
}

/// Мелочь для источников: миллисекунды в Duration без переполнений.
pub(crate) fn ms(v: u64) -> Duration {
    Duration::from_millis(v)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kinds_by_extension() {
        assert_eq!(kind_for(Path::new("a.GIF")), Some(Kind::Gif));
        assert_eq!(kind_for(Path::new("/x/y.jpeg")), Some(Kind::Still));
        assert_eq!(kind_for(Path::new("clip.MKV")), Some(Kind::Video));
        assert_eq!(kind_for(Path::new("clip.avi")), None);
        assert_eq!(kind_for(Path::new("noext")), None);
    }
}
