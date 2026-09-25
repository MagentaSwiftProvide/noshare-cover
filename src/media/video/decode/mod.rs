//! Декодеры. Программные вшиты (rav1d) или грузятся из системы на лету
//! (openh264, libvpx), аппаратные — VA-API и NVDEC — тоже через dlopen:
//! плагин не тянет жёстких зависимостей и не падает, если библиотеки нет.

#[cfg(feature = "cpu-av1")]
pub mod av1;
#[cfg(feature = "cpu-h264")]
pub mod h264;
#[cfg(feature = "nvdec")]
pub mod nvdec;
#[cfg(all(feature = "vaapi", target_os = "linux"))]
pub mod vaapi;
#[cfg(feature = "cpu-vpx")]
pub mod vpx;
