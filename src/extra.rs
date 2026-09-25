//! Дополнительные прямоугольники от других плагинов (например, gloview:
//! плитки превью окон с no_screen_share в оверлее).
//!
//! API v2 (`include/noshare_cover_api.h`):
//! - у каждого плагина свой клиент: `register` → id, свои прямоугольники никто
//!   чужой не сотрёт;
//! - `set_rects(client, monitor, rects[])` атомарно заменяет набор клиента на
//!   мониторе — нет окна «уже очистил, ещё не добавил», значит нет мерцания;
//! - заливка: чёрным или обложкой конкретного окна (по адресу окна Hyprland,
//!   как в `hyprctl clients`) — плитка в оверлее показывает ту же картинку,
//!   что и само окно в скриншере;
//! - `unregister` снимает всё клиента (вызывать при выгрузке плагина).
//!
//! API v1 (`nsc_api_clear_extra_rects` / `nsc_api_add_extra_rect`)
//! работает как раньше: это клиент «legacy» с чёрной заливкой.

use std::sync::{Mutex, MutexGuard};

pub const API_VERSION: u32 = 2;

/// Чем залить прямоугольник.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Fill {
    Black = 0,
    /// Обложка окна `window`; если окна нет или у него нет обложки — чёрным.
    WindowCover = 1,
}

impl Fill {
    fn from_raw(v: u32) -> Self {
        if v == Fill::WindowCover as u32 {
            Fill::WindowCover
        } else {
            Fill::Black
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CoverRect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub rounding: f64,
    /// Адрес окна Hyprland (как `address` в `hyprctl clients`), 0 — нет окна.
    pub window: u64,
    pub fill: Fill,
}

impl CoverRect {
    fn sane(&self) -> Option<Self> {
        let finite = [self.x, self.y, self.w, self.h]
            .iter()
            .all(|v| v.is_finite());
        (finite && self.w > 0.0 && self.h > 0.0).then(|| Self {
            rounding: if self.rounding.is_finite() {
                self.rounding.max(0.0)
            } else {
                0.0
            },
            ..*self
        })
    }
}

/// Прямоугольник, как его видит отрисовка (с монитором).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MonitorRect {
    pub monitor: i64,
    pub rect: CoverRect,
}

#[derive(Debug)]
struct Client {
    id: u64,
    name: String,
    rects: Vec<MonitorRect>,
}

#[derive(Debug)]
struct Clients {
    list: Vec<Client>,
    next_id: u64,
}

/// id клиента для старого API v1.
pub const LEGACY_CLIENT: u64 = 1;
/// Сколько прямоугольников держим на клиента: защита от утечки у чужого плагина.
const MAX_RECTS_PER_CLIENT: usize = 4096;

static CLIENTS: Mutex<Clients> = Mutex::new(Clients {
    list: Vec::new(),
    next_id: LEGACY_CLIENT + 1,
});

fn clients() -> MutexGuard<'static, Clients> {
    // отравленный мьютекс тут не опасен: внутри только данные, берём как есть
    CLIENTS.lock().unwrap_or_else(|e| e.into_inner())
}

fn client_mut(c: &mut Clients, id: u64) -> Option<&mut Client> {
    c.list.iter_mut().find(|cl| cl.id == id)
}

fn legacy(c: &mut Clients) -> &mut Client {
    if !c.list.iter().any(|cl| cl.id == LEGACY_CLIENT) {
        c.list.push(Client {
            id: LEGACY_CLIENT,
            name: "legacy".into(),
            rects: Vec::new(),
        });
    }
    client_mut(c, LEGACY_CLIENT).expect("legacy client exists")
}

/// Новый клиент. Имя — для отладки (кто насорил прямоугольниками).
pub fn register(name: &str) -> u64 {
    let mut c = clients();
    let id = c.next_id;
    c.next_id += 1;
    c.list.push(Client {
        id,
        name: name.chars().take(64).collect(),
        rects: Vec::new(),
    });
    id
}

pub fn unregister(id: u64) {
    clients().list.retain(|cl| cl.id != id);
}

/// Заменить все прямоугольники клиента на мониторе. Пустой список — очистить монитор.
/// `false` — клиента нет (не зарегистрирован или уже снят).
pub fn set_rects(id: u64, monitor: i64, rects: &[CoverRect]) -> bool {
    let mut c = clients();
    let Some(cl) = client_mut(&mut c, id) else {
        return false;
    };
    cl.rects.retain(|r| r.monitor != monitor);
    let room = MAX_RECTS_PER_CLIENT.saturating_sub(cl.rects.len());
    cl.rects.extend(
        rects
            .iter()
            .filter_map(CoverRect::sane)
            .take(room)
            .map(|rect| MonitorRect { monitor, rect }),
    );
    true
}

/// Очистить все мониторы клиента, не снимая регистрацию.
pub fn clear_client(id: u64) -> bool {
    let mut c = clients();
    match client_mut(&mut c, id) {
        Some(cl) => {
            cl.rects.clear();
            true
        }
        None => false,
    }
}

/// Все прямоугольники монитора от всех клиентов (копия — замок не держим во время отрисовки).
pub fn for_monitor(monitor: i64) -> Vec<CoverRect> {
    clients()
        .list
        .iter()
        .flat_map(|cl| cl.rects.iter())
        .filter(|r| r.monitor == monitor)
        .map(|r| r.rect)
        .collect()
}

/// Сброс при выгрузке плагина.
pub fn reset() {
    let mut c = clients();
    c.list.clear();
    c.next_id = LEGACY_CLIENT + 1;
}

/// Для отладки: кто сколько держит.
pub fn debug_summary() -> Vec<(String, usize)> {
    clients()
        .list
        .iter()
        .map(|cl| (cl.name.clone(), cl.rects.len()))
        .collect()
}

// ---------------------------------------------------------------- API v1

pub fn legacy_clear() {
    legacy(&mut clients()).rects.clear();
}

pub fn legacy_add(monitor: i64, x: f64, y: f64, w: f64, h: f64, rounding: f64) {
    let rect = CoverRect {
        x,
        y,
        w,
        h,
        rounding,
        window: 0,
        fill: Fill::Black,
    };
    let Some(rect) = rect.sane() else { return };
    let mut c = clients();
    let cl = legacy(&mut c);
    if cl.rects.len() < MAX_RECTS_PER_CLIENT {
        cl.rects.push(MonitorRect { monitor, rect });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_clear_extra_rects() {
    let _ = std::panic::catch_unwind(legacy_clear);
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_add_extra_rect(
    monitor_id: i32,
    x: f64,
    y: f64,
    w: f64,
    h: f64,
    rounding: f64,
) {
    let _ = std::panic::catch_unwind(|| legacy_add(i64::from(monitor_id), x, y, w, h, rounding));
}

// ---------------------------------------------------------------- API v2 (C)

/// `noshare_cover_rect` из публичного заголовка.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CRect {
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub rounding: f64,
    pub window: u64,
    pub fill: u32,
}

impl From<&CRect> for CoverRect {
    fn from(r: &CRect) -> Self {
        Self {
            x: r.x,
            y: r.y,
            w: r.w,
            h: r.h,
            rounding: r.rounding,
            window: r.window,
            fill: Fill::from_raw(r.fill),
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_api_version() -> u32 {
    API_VERSION
}

/// # Safety
/// `name` — NUL-строка или NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_api_register_client(name: *const std::ffi::c_char) -> u64 {
    std::panic::catch_unwind(|| {
        let name = if name.is_null() {
            String::from("?")
        } else {
            unsafe { std::ffi::CStr::from_ptr(name) }
                .to_string_lossy()
                .into_owned()
        };
        register(&name)
    })
    .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_unregister_client(client: u64) {
    let _ = std::panic::catch_unwind(|| unregister(client));
}

/// # Safety
/// `rects` указывает на `count` элементов (или NULL при `count == 0`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nsc_api_set_rects(
    client: u64,
    monitor_id: i32,
    rects: *const CRect,
    count: usize,
) -> bool {
    std::panic::catch_unwind(|| {
        let slice = if rects.is_null() || count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(rects, count) }
        };
        let converted: Vec<CoverRect> = slice.iter().map(CoverRect::from).collect();
        set_rects(client, i64::from(monitor_id), &converted)
    })
    .unwrap_or(false)
}

#[unsafe(no_mangle)]
pub extern "C" fn nsc_api_clear_client_rects(client: u64) -> bool {
    std::panic::catch_unwind(|| clear_client(client)).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn r(x: f64) -> CoverRect {
        CoverRect {
            x,
            y: 0.0,
            w: 10.0,
            h: 10.0,
            rounding: 0.0,
            window: 0,
            fill: Fill::Black,
        }
    }

    // Глобальное состояние — весь сценарий одним тестом, чтобы параллельные тесты не мешали.
    #[test]
    fn api_scenarios() {
        reset();

        // v1 ведёт себя как раньше
        nsc_api_add_extra_rect(1, 10.0, 10.0, 100.0, 50.0, -3.0);
        nsc_api_add_extra_rect(1, 0.0, 0.0, 0.0, 50.0, 0.0); // пустой — отброшен
        nsc_api_add_extra_rect(1, f64::NAN, 0.0, 10.0, 10.0, 0.0); // мусор — отброшен
        let m1 = for_monitor(1);
        assert_eq!(m1.len(), 1);
        assert_eq!(m1[0].rounding, 0.0);

        // v2: у клиента свой набор, чужой clear его не трогает
        let a = register("gloview");
        let b = register("other");
        assert!(set_rects(a, 1, &[r(1.0), r(2.0)]));
        assert!(set_rects(b, 1, &[r(3.0)]));
        assert_eq!(for_monitor(1).len(), 4);
        nsc_api_clear_extra_rects(); // v1 clear чистит только legacy
        assert_eq!(for_monitor(1).len(), 3);

        // set_rects атомарно заменяет набор на мониторе, другие мониторы целы
        assert!(set_rects(a, 2, &[r(9.0)]));
        assert!(set_rects(a, 1, &[r(5.0)]));
        let xs: Vec<f64> = for_monitor(1).iter().map(|c| c.x).collect();
        assert!(
            xs.contains(&5.0) && !xs.contains(&1.0) && xs.contains(&3.0),
            "{xs:?}"
        );
        assert_eq!(for_monitor(2).len(), 1);

        // заливка обложкой окна проходит насквозь
        let cover = CRect {
            x: 0.0,
            y: 0.0,
            w: 5.0,
            h: 5.0,
            rounding: 2.0,
            window: 0xdead,
            fill: 1,
        };
        assert!(unsafe { nsc_api_set_rects(a, 3, &cover, 1) });
        let got = for_monitor(3);
        assert_eq!((got[0].fill, got[0].window), (Fill::WindowCover, 0xdead));
        // неизвестная заливка = чёрная
        let odd = CRect { fill: 77, ..cover };
        assert!(unsafe { nsc_api_set_rects(a, 3, &odd, 1) });
        assert_eq!(for_monitor(3)[0].fill, Fill::Black);

        // снятый клиент больше ничего не держит и писать не может
        unregister(a);
        assert!(!set_rects(a, 1, &[r(1.0)]));
        assert_eq!(for_monitor(1).len(), 1); // остался только b
        assert!(clear_client(b));
        assert!(for_monitor(1).is_empty());

        // лимит на клиента
        let c = register("spammer");
        let many = vec![r(1.0); MAX_RECTS_PER_CLIENT + 100];
        assert!(set_rects(c, 1, &many));
        assert_eq!(for_monitor(1).len(), MAX_RECTS_PER_CLIENT);

        assert_eq!(nsc_api_api_version(), 2);
        reset();
        assert!(for_monitor(1).is_empty());
    }
}
