//! noshare-cover: ядро плагина Hyprland на Rust.
//!
//! Hyprland не даёт плагинам C-API, только свой C++ ABI, который собирается под
//! конкретную версию композитора. Поэтому тут вся логика (медиа, декод, часы
//! воспроизведения, конфиг, жизненный цикл ресурсов, extra rects), а тонкая
//! прослойка `shim/plugin.cpp` только регистрирует хук, читает конфиг и правила
//! окон, превращает готовые кадры в текстуры Hyprland и рисует их.
//!
//! Граница между ними — C ABI из [`ffi`], описанный в `include/noshare_cover.h`.

pub mod clock;
pub mod config;
pub mod extra;
pub mod ffi;
pub mod frame;
pub mod gpu;
pub mod media;
pub mod notify;
pub mod registry;

pub use config::{Backend, PlayParams, Settings};
pub use frame::{Frame, FrameData};
pub use registry::Registry;
