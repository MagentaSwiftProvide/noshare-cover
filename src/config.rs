//! Настройки плагина и параметры воспроизведения для конкретного окна.
//!
//! Глобальные значения живут в `plugin:no_screen_share_cover:*`, окно может
//! переопределить `path_cover`, `speed` и `loop` правилом. Бэкенд декода и
//! GPU задаются только глобально: это свойство машины, а не окна.

use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};

/// Где декодировать видео.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Backend {
    /// GPU, если он есть и умеет кодек, иначе CPU.
    #[default]
    Auto,
    /// Только GPU. Не получилось — ошибка, без тихого отката на CPU.
    Gpu,
    /// Только CPU.
    Cpu,
}

impl Backend {
    /// Разбор значения из конфига. Неизвестное слово — `None`, чтобы вызывающий
    /// мог показать понятную ошибку, а не молча выбрать что-то своё.
    pub fn parse(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "" | "auto" => Some(Self::Auto),
            "gpu" | "hw" | "hardware" => Some(Self::Gpu),
            "cpu" | "sw" | "software" => Some(Self::Cpu),
            _ => None,
        }
    }
}

/// Глобальные настройки плагина (уже разобранные и проверенные).
#[derive(Debug, Clone, PartialEq)]
pub struct Settings {
    /// Медиа по умолчанию для окон без своего правила.
    pub path_cover: PathBuf,
    pub looped: bool,
    pub speed: f64,
    pub backend: Backend,
    /// Конкретный render node (`/dev/dri/renderD129`). `None` — выбрать самим.
    pub gpu_device: Option<PathBuf>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            path_cover: default_media_path(),
            looped: true,
            speed: 1.0,
            backend: Backend::Auto,
            gpu_device: None,
        }
    }
}

/// Сырые значения, как их отдаёт конфиг Hyprland.
#[derive(Debug, Clone, Default)]
pub struct RawSettings<'a> {
    pub path_cover: &'a str,
    pub looped: bool,
    pub speed: f64,
    pub backend: &'a str,
    pub gpu_device: &'a str,
}

/// Ошибка в конфиге: показываем пользователю, работаем на значениях по умолчанию.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ConfigError {
    #[error("noshare-cover: неизвестный backend «{0}», жду auto, gpu или cpu")]
    UnknownBackend(String),
}

impl Settings {
    pub fn from_raw(raw: &RawSettings<'_>) -> (Self, Option<ConfigError>) {
        let mut err = None;
        let backend = Backend::parse(raw.backend).unwrap_or_else(|| {
            err = Some(ConfigError::UnknownBackend(raw.backend.to_owned()));
            Backend::Auto
        });

        let path_cover = if raw.path_cover.trim().is_empty() {
            default_media_path()
        } else {
            expand_home(raw.path_cover.trim())
        };

        let gpu_device = match raw.gpu_device.trim() {
            "" | "auto" => None,
            dev => Some(expand_home(dev)),
        };

        let settings = Self {
            path_cover,
            looped: raw.looped,
            speed: sanitize_speed(raw.speed),
            backend,
            gpu_device,
        };
        (settings, err)
    }

    /// Параметры воспроизведения по умолчанию (без правил окна).
    pub fn default_play(&self) -> PlayParams {
        PlayParams {
            path: self.path_cover.clone(),
            speed: self.speed,
            looped: self.looped,
        }
    }
}

/// Что и как играть в конкретном окне. Одинаковые параметры делят один декодер
/// и одну текстуру, сколько бы окон их ни показывало.
#[derive(Debug, Clone)]
pub struct PlayParams {
    pub path: PathBuf,
    pub speed: f64,
    pub looped: bool,
}

impl PlayParams {
    /// Применить переопределения из правила окна. Пустое значение — не трогать.
    pub fn with_rule(
        mut self,
        path: Option<&str>,
        speed: Option<&str>,
        looped: Option<&str>,
    ) -> Self {
        if let Some(p) = path.map(str::trim).filter(|p| !p.is_empty()) {
            self.path = expand_home(p);
        }
        if let Some(s) = speed.map(str::trim).filter(|s| !s.is_empty()) {
            self.speed = sanitize_speed(s.parse().unwrap_or(1.0));
        }
        if let Some(l) = looped.map(str::trim).filter(|l| !l.is_empty()) {
            self.looped = truthy(l);
        }
        self
    }
}

// speed сравниваем побитово: 1.0 и 1.0000001 — разные ключи, и это нормально,
// NaN сюда не попадает (sanitize_speed).
impl PartialEq for PlayParams {
    fn eq(&self, other: &Self) -> bool {
        self.path == other.path
            && self.speed.to_bits() == other.speed.to_bits()
            && self.looped == other.looped
    }
}
impl Eq for PlayParams {}
impl Hash for PlayParams {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.path.hash(state);
        self.speed.to_bits().hash(state);
        self.looped.hash(state);
    }
}

/// Скорость ≤ 0, NaN и бесконечность превращаются в 1.0.
pub fn sanitize_speed(speed: f64) -> f64 {
    if speed.is_finite() && speed > 0.0 {
        speed
    } else {
        1.0
    }
}

/// Как в исходном плагине: всё, кроме 0/false/no/off, — правда.
pub fn truthy(raw: &str) -> bool {
    !matches!(
        raw.trim().to_ascii_lowercase().as_str(),
        "0" | "false" | "no" | "off"
    )
}

pub fn home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME")
        .filter(|h| !h.is_empty())
        .map(PathBuf::from)
}

/// `~` и `~/…` раскрываются в домашний каталог, остальное как есть.
pub fn expand_home(path: &str) -> PathBuf {
    if path == "~" {
        return home_dir().unwrap_or_else(|| PathBuf::from(path));
    }
    if let Some(rest) = path.strip_prefix("~/")
        && let Some(home) = home_dir()
    {
        return home.join(rest);
    }
    PathBuf::from(path)
}

/// Файл по умолчанию: первый существующий из списка в `~/.config/hypr`, как раньше.
pub fn default_media_path() -> PathBuf {
    default_media_path_in(&config_dir())
}

fn config_dir() -> PathBuf {
    home_dir().unwrap_or_default().join(".config").join("hypr")
}

fn default_media_path_in(dir: &Path) -> PathBuf {
    const NAMES: [&str; 5] = [
        "noshare-cover.gif",
        "noshare-cover.jpg",
        "noshare-cover.jpeg",
        "noshare-cover.png",
        "noshare-cover.mp4",
    ];
    NAMES
        .iter()
        .map(|name| dir.join(name))
        .find(|p| p.exists())
        .unwrap_or_else(|| dir.join(NAMES[0]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backend_parse() {
        assert_eq!(Backend::parse(""), Some(Backend::Auto));
        assert_eq!(Backend::parse(" GPU "), Some(Backend::Gpu));
        assert_eq!(Backend::parse("cpu"), Some(Backend::Cpu));
        assert_eq!(Backend::parse("cuda"), None);
    }

    #[test]
    fn unknown_backend_falls_back_with_error() {
        let raw = RawSettings {
            path_cover: "/x.gif",
            looped: true,
            speed: 1.0,
            backend: "vulkan",
            gpu_device: "",
        };
        let (s, err) = Settings::from_raw(&raw);
        assert_eq!(s.backend, Backend::Auto);
        assert_eq!(err, Some(ConfigError::UnknownBackend("vulkan".into())));
    }

    #[test]
    fn speed_is_sanitized() {
        assert_eq!(sanitize_speed(0.0), 1.0);
        assert_eq!(sanitize_speed(-2.0), 1.0);
        assert_eq!(sanitize_speed(f64::NAN), 1.0);
        assert_eq!(sanitize_speed(2.5), 2.5);
    }

    #[test]
    fn truthy_matches_original_plugin() {
        for f in ["0", "false", "No", " off "] {
            assert!(!truthy(f), "{f}");
        }
        for t in ["1", "true", "yes", "on", "whatever"] {
            assert!(truthy(t), "{t}");
        }
    }

    #[test]
    fn rule_overrides_only_non_empty() {
        let base = PlayParams {
            path: "/a.gif".into(),
            speed: 1.0,
            looped: true,
        };
        let p = base.clone().with_rule(Some(""), Some("2"), Some("off"));
        assert_eq!(p.path, PathBuf::from("/a.gif"));
        assert_eq!(p.speed, 2.0);
        assert!(!p.looped);
        let bad = base.with_rule(None, Some("-1"), None);
        assert_eq!(bad.speed, 1.0);
    }

    #[test]
    fn default_media_prefers_first_existing() {
        let dir = std::env::temp_dir().join(format!("nsc-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        assert_eq!(default_media_path_in(&dir), dir.join("noshare-cover.gif"));
        std::fs::write(dir.join("noshare-cover.png"), b"x").unwrap();
        assert_eq!(default_media_path_in(&dir), dir.join("noshare-cover.png"));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
