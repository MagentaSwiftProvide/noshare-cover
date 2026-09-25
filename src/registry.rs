//! Cover registry: one source per unique (file, speed, loop), no matter how
//! many windows show it.
//!
//! Lifecycle:
//! - global settings change: everything is reset and the epoch is bumped (the
//!   shim drops its textures on a new epoch);
//! - file missing: retry once per second, no stat() on every frame;
//! - failed to open: one notification, no retries until the config changes;
//! - cover not shown for a while: the source is closed (for video this stops
//!   the decode thread) instead of living until the plugin is unloaded.

use std::collections::HashMap;
use std::time::{Duration, Instant};

use crate::config::{ConfigError, PlayParams, Settings};
use crate::frame::Frame;
use crate::media::{self, MediaError, Source};
use crate::notify::Notifier;

/// Opens a source for the given parameters (replaced in tests).
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

/// What the shim draws: a stable cover id (texture cache key) and the frame.
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

    /// New global settings. If they changed, reset everything.
    pub fn set_settings(&mut self, settings: Settings, error: Option<ConfigError>) {
        if self.settings.as_ref() == Some(&settings) {
            return;
        }
        self.covers.clear(); // dropping sources stops the video threads
        self.settings = Some(settings);
        self.epoch += 1;
        self.notifier.reset();
        if let Some(e) = error {
            self.notifier.push(e.to_string());
        }
        self.prewarm(Instant::now());
    }

    /// Open the default cover up front so the first screen capture already has
    /// a frame ready. The video then sleeps (nobody is watching) but keeps its
    /// last frame; eviction never touches it.
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

    /// Cover for a window. `None` means nothing to draw (no file, error, no frame yet).
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
            // a cover shown in several windows is polled once per frame
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

    /// End of frame: close covers nobody has shown for a while.
    pub fn end_frame(&mut self, now: Instant) {
        let default = self.settings().default_play();
        self.covers.retain(|play, c| {
            *play == default || now.saturating_duration_since(c.last_used) < EVICT_AFTER
        });
    }

    /// Whether any live animated cover (video, GIF) exists. The shim uses this
    /// to decide whether to nudge Hyprland into new capture frames. Covers not
    /// shown for a while get evicted after EVICT_AFTER anyway.
    pub fn animating(&self, now: Instant) -> bool {
        self.covers.values().any(|c| {
            matches!(&c.state, State::Ready(src) if src.kind() != crate::media::Kind::Still)
                && now.saturating_duration_since(c.last_used) < EVICT_AFTER
        })
    }

    /// Live cover ids (the shim frees textures for ids not listed here).
    pub fn live_ids(&self) -> impl Iterator<Item = u64> + '_ {
        self.covers.values().map(|c| c.id)
    }

    fn open(&mut self, play: &PlayParams, settings: &Settings, now: Instant) -> State {
        match (self.opener)(play, settings) {
            Ok(s) => State::Ready(s),
            Err(MediaError::Missing(p)) => {
                self.notifier
                    .push(format!("noshare-cover: file not found: {p}"));
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

    // per-thread counter: tests run in parallel, a shared static would cause spurious failures
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

    /// Registry with settings; the default cover (/default) is already prewarmed.
    fn reg() -> Registry {
        let mut r = Registry::with_opener(opener);
        r.set_settings(defaults(), None);
        r
    }

    #[test]
    fn default_cover_is_prewarmed_and_kept() {
        let r = reg();
        assert_eq!(r.len(), 1, "default cover is opened right away");
        let mut r = r;
        r.end_frame(Instant::now() + EVICT_AFTER * 3);
        assert_eq!(
            r.len(),
            1,
            "and never evicted, even if unused for a long time"
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
            "everything reset, only the default cover is prewarmed again"
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
        assert_eq!(r.len(), 1, "/a evicted, /default kept");
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
        // first open only; retries happen after the backoff and only if the file appears
        assert_eq!(opens() - before, 1);
    }
}
