//! Реестр обложек: один источник на уникальные (файл, скорость, петля),
//! сколько бы окон его ни показывало.
//!
//! Жизненный цикл:
//! - смена глобальных настроек — всё сбрасывается, эпоха растёт (прослойка по
//!   эпохе выкидывает свои текстуры);
//! - нет файла — пробуем снова раз в секунду, без stat() на каждый кадр;
//! - не открылся — одно уведомление, повторов нет до смены конфига;
//! - обложку давно не показывали — источник закрывается (для видео это
//!   остановка потока декода), а не живёт до выгрузки плагина.

use std::collections::HashMap;
use std::time::{Duration, Instant};

use crate::config::{ConfigError, PlayParams, Settings};
use crate::frame::Frame;
use crate::media::{self, MediaError, Source};
use crate::notify::Notifier;

/// Как открыть источник по параметрам (в тестах подменяется).
pub type Opener = fn(&PlayParams, &Settings) -> Result<Box<dyn Source>, MediaError>;

const MISSING_RETRY: Duration = Duration::from_secs(1);
const EVICT_AFTER: Duration = Duration::from_secs(30);

enum State {
    Ready(Box<dyn Source>),
    Missing { retry_at: Instant },
    Failed,
}

struct Cover {
    id: u64,
    state: State,
    current: Option<Frame>,
    polled_in: u64,
    last_used: Instant,
}

/// То, что прослойка рисует: стабильный id обложки (ключ кэша текстур) и кадр.
pub struct CoverView<'a> {
    pub id: u64,
    pub frame: &'a Frame,
}

pub struct Registry {
    settings: Option<Settings>,
    epoch: u64,
    covers: HashMap<PlayParams, Cover>,
    next_id: u64,
    frame_no: u64,
    notifier: Notifier,
    opener: Opener,
}

impl Default for Registry {
    fn default() -> Self {
        Self::with_opener(media::open)
    }
}

impl Registry {
    pub fn with_opener(opener: Opener) -> Self {
        Self {
            settings: None,
            epoch: 0,
            covers: HashMap::new(),
            next_id: 1,
            frame_no: 0,
            notifier: Notifier::default(),
            opener,
        }
    }

    pub fn epoch(&self) -> u64 {
        self.epoch
    }

    pub fn settings(&self) -> Settings {
        self.settings.clone().unwrap_or_default()
    }

    pub fn notifier(&mut self) -> &mut Notifier {
        &mut self.notifier
    }

    /// Новые глобальные настройки. Если они поменялись — сбрасываем всё.
    pub fn set_settings(&mut self, settings: Settings, error: Option<ConfigError>) {
        if self.settings.as_ref() == Some(&settings) {
            return;
        }
        self.covers.clear(); // drop источников = остановка потоков видео
        self.settings = Some(settings);
        self.epoch += 1;
        self.notifier.reset();
        if let Some(e) = error {
            self.notifier.push(e.to_string());
        }
        self.prewarm(Instant::now());
    }

    /// Открыть обложку по умолчанию заранее, чтобы первый захват экрана уже
    /// застал готовый кадр. Видео после этого спит (никто не смотрит), но
    /// последний кадр держит; вытеснение её не трогает.
    fn prewarm(&mut self, now: Instant) {
        let play = self.settings().default_play();
        if play.path.as_os_str().is_empty() {
            return;
        }
        self.begin_frame();
        let _ = self.resolve(&play, now);
    }

    pub fn begin_frame(&mut self) {
        self.frame_no += 1;
    }

    /// Обложка для окна. `None` — рисовать нечего (нет файла, ошибка, ещё нет кадра).
    pub fn resolve(&mut self, play: &PlayParams, now: Instant) -> Option<CoverView<'_>> {
        let settings = self.settings();
        let frame_no = self.frame_no;

        if !self.covers.contains_key(play) {
            let id = self.next_id;
            self.next_id += 1;
            let state = self.open(play, &settings, now);
            self.covers.insert(
                play.clone(),
                Cover {
                    id,
                    state,
                    current: None,
                    polled_in: 0,
                    last_used: now,
                },
            );
        }

        let cover = self.covers.get_mut(play)?;
        cover.last_used = now;

        if let State::Missing { retry_at } = cover.state {
            if now >= retry_at && play.path.exists() {
                let opener = self.opener;
                cover.state = match opener(play, &settings) {
                    Ok(s) => State::Ready(s),
                    Err(e) => {
                        self.notifier.push(format!("noshare-cover: {e}"));
                        State::Failed
                    }
                };
            } else if now >= retry_at {
                cover.state = State::Missing {
                    retry_at: now + MISSING_RETRY,
                };
            }
        }

        if let State::Ready(src) = &mut cover.state {
            // одна обложка на нескольких окнах опрашивается один раз за кадр
            if cover.polled_in != frame_no {
                cover.polled_in = frame_no;
                match src.poll(now) {
                    Ok(Some(f)) => cover.current = Some(f),
                    Ok(None) => {}
                    Err(e) => {
                        self.notifier.push(format!("noshare-cover: {e}"));
                        cover.state = State::Failed;
                        cover.current = None;
                    }
                }
            }
        }

        let cover = self.covers.get(play)?;
        cover.current.as_ref().map(|frame| CoverView {
            id: cover.id,
            frame,
        })
    }

    /// Конец кадра: закрываем то, что давно никто не показывал.
    pub fn end_frame(&mut self, now: Instant) {
        let default = self.settings().default_play();
        self.covers.retain(|play, c| {
            *play == default || now.saturating_duration_since(c.last_used) < EVICT_AFTER
        });
    }

    /// Есть ли недавно показанная анимированная обложка (видео, GIF). Прослойка
    /// по этому решает, подталкивать ли Hyprland к новым кадрам захвата.
    pub fn animating(&self, now: Instant) -> bool {
        self.covers.values().any(|c| {
            matches!(&c.state, State::Ready(src) if src.kind() != crate::media::Kind::Still)
                && now.saturating_duration_since(c.last_used) < Duration::from_secs(1)
        })
    }

    /// Живые id обложек (прослойка чистит текстуры тех, кого тут нет).
    pub fn live_ids(&self) -> impl Iterator<Item = u64> + '_ {
        self.covers.values().map(|c| c.id)
    }

    fn open(&mut self, play: &PlayParams, settings: &Settings, now: Instant) -> State {
        match (self.opener)(play, settings) {
            Ok(s) => State::Ready(s),
            Err(MediaError::Missing(p)) => {
                self.notifier.push(format!("noshare-cover: нет файла {p}"));
                State::Missing {
                    retry_at: now + MISSING_RETRY,
                }
            }
            Err(e) => {
                self.notifier.push(format!("noshare-cover: {e}"));
                State::Failed
            }
        }
    }

    pub fn len(&self) -> usize {
        self.covers.len()
    }

    pub fn is_empty(&self) -> bool {
        self.covers.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frame::{CpuFrame, FrameData};
    use crate::media::Kind;
    use std::cell::Cell;

    // счётчик на поток: тесты идут параллельно, общий static дал бы ложные падения
    thread_local!(static OPENS: Cell<usize> = const { Cell::new(0) });

    fn opens() -> usize {
        OPENS.with(Cell::get)
    }

    struct Solid(u64);
    impl Source for Solid {
        fn kind(&self) -> Kind {
            Kind::Still
        }
        fn poll(&mut self, _: Instant) -> Result<Option<Frame>, MediaError> {
            self.0 += 1;
            Ok(Some(Frame {
                generation: self.0,
                data: FrameData::Cpu(CpuFrame::from_bgra(1, 1, vec![0; 4].into())),
            }))
        }
    }

    fn opener(p: &PlayParams, _: &Settings) -> Result<Box<dyn Source>, MediaError> {
        OPENS.with(|c| c.set(c.get() + 1));
        match p.path.to_str().unwrap() {
            "/missing" => Err(MediaError::Missing("/missing".into())),
            "/bad" => Err(MediaError::UnknownFormat(".xyz".into())),
            _ => Ok(Box::new(Solid(0))),
        }
    }

    fn play(path: &str) -> PlayParams {
        PlayParams {
            path: path.into(),
            speed: 1.0,
            looped: true,
        }
    }

    fn defaults() -> Settings {
        Settings {
            path_cover: "/default".into(),
            ..Settings::default()
        }
    }

    /// Реестр с настройками; обложка по умолчанию (/default) уже прогрета.
    fn reg() -> Registry {
        let mut r = Registry::with_opener(opener);
        r.set_settings(defaults(), None);
        r
    }

    #[test]
    fn default_cover_is_prewarmed_and_kept() {
        let r = reg();
        assert_eq!(r.len(), 1, "обложка по умолчанию открыта сразу");
        let mut r = r;
        r.end_frame(Instant::now() + EVICT_AFTER * 3);
        assert_eq!(
            r.len(),
            1,
            "и не вытесняется, даже если её долго не показывали"
        );
    }

    #[test]
    fn shared_cover_polled_once_per_frame() {
        let mut r = reg();
        let now = Instant::now();
        r.begin_frame();
        let g1 = r.resolve(&play("/a"), now).unwrap().frame.generation;
        let g2 = r.resolve(&play("/a"), now).unwrap().frame.generation;
        assert_eq!(g1, g2, "second window in same frame reuses the frame");
        r.begin_frame();
        assert_eq!(
            r.resolve(&play("/a"), now).unwrap().frame.generation,
            g1 + 1
        );
        assert_eq!(r.len(), 2, "/default + /a");
    }

    #[test]
    fn errors_notify_once() {
        let mut r = reg();
        let now = Instant::now();
        for _ in 0..3 {
            r.begin_frame();
            assert!(r.resolve(&play("/bad"), now).is_none());
        }
        assert!(r.notifier().pop().is_some());
        assert!(r.notifier().pop().is_none());
    }

    #[test]
    fn settings_change_resets_everything() {
        let mut r = reg();
        let now = Instant::now();
        r.begin_frame();
        let id = r.resolve(&play("/a"), now).unwrap().id;
        let epoch = r.epoch();
        r.set_settings(
            Settings {
                speed: 2.0,
                ..defaults()
            },
            None,
        );
        assert_eq!(r.epoch(), epoch + 1);
        assert_eq!(
            r.len(),
            1,
            "всё сброшено, заново прогрета только обложка по умолчанию"
        );
        r.begin_frame();
        assert_ne!(
            r.resolve(&play("/a"), now).unwrap().id,
            id,
            "new cover id after reset"
        );
    }

    #[test]
    fn same_settings_do_not_reset() {
        let mut r = reg();
        let e = r.epoch();
        r.set_settings(defaults(), None);
        assert_eq!(r.epoch(), e);
    }

    #[test]
    fn unused_covers_are_evicted() {
        let mut r = reg();
        let now = Instant::now();
        r.begin_frame();
        r.resolve(&play("/a"), now);
        r.end_frame(now + Duration::from_secs(5));
        assert_eq!(r.len(), 2);
        r.end_frame(now + EVICT_AFTER + Duration::from_secs(1));
        assert_eq!(r.len(), 1, "/a вытеснена, /default осталась");
    }

    #[test]
    fn missing_file_is_retried_with_backoff() {
        let mut r = reg();
        let now = Instant::now();
        let before = opens();
        for i in 0..10 {
            r.begin_frame();
            r.resolve(&play("/missing"), now + Duration::from_millis(i * 10));
        }
        // первое открытие; повторы — только после бэкоффа и только если файл появился
        assert_eq!(opens() - before, 1);
    }
}
