// noshare-cover: тонкая прослойка между Hyprland и Rust-ядром.
//
// Здесь только то, что без C++ ABI Hyprland не сделать:
//   - хук CScreenshareFrame::renderMonitor (кадр скриншера);
//   - регистрация значений конфига и эффектов правил окон;
//   - геометрия окон и отрисовка через render pass;
//   - превращение кадров ядра в текстуры (пиксели или dmabuf, без cairo).
// Вся логика медиа, декода, часов и жизненного цикла — в Rust (src/).

#include "../include/noshare_cover.h"

#include <drm_fourcc.h>

#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "config/values/ConfigValues.hpp"
#include "desktop/rule/Engine.hpp"
#include "desktop/rule/windowRule/WindowRule.hpp"
#include "desktop/state/WindowState.hpp"
// main (0.57-dev) разнёс окно на Window + WindowPresentation; релиз 0.56 — ещё одним классом.
#if __has_include("desktop/view/window/Window.hpp")
#include "desktop/view/window/Window.hpp"
#include "desktop/view/window/WindowPresentation.hpp"
#define NSC_SPLIT_WINDOW 1
#else
#include "desktop/view/Window.hpp"
#endif
#include "event/EventBus.hpp"
#include "managers/eventLoop/EventLoopTimer.hpp"
#include "managers/fullscreen/FullscreenController.hpp"
#include "plugins/PluginAPI.hpp"
#include "protocols/types/Buffer.hpp"
#include "render/Framebuffer.hpp"
#include "render/Renderer.hpp"
#include "render/pass/RectPassElement.hpp"
#include "render/pass/TexPassElement.hpp"

#include <aquamarine/buffer/Buffer.hpp>

// m_session у кадра скриншера приватный, публичного события для отрисовки в
// кадр скриншера upstream не даёт. Всё, что этот заголовок тянет, уже включено
// выше, так что `private public` касается только его собственных классов.
#define private public
#include "managers/screenshare/ScreenshareManager.hpp"
#undef private

namespace {

    // Разница API окна между релизом и main — только здесь.
    float windowFade(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
#else
        return w->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
#endif
    }

    bool windowPinned(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return static_cast<bool>(w->m_state & Desktop::View::WINDOW_STATE_PINNED);
#else
        return w->m_pinned;
#endif
    }

    float windowRounding(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return w->presentation().rounding();
#else
        return w->rounding();
#endif
    }

    float windowRoundingPower(const PHLWINDOW& w) {
#ifdef NSC_SPLIT_WINDOW
        return w->presentation().roundingPower();
#else
        return w->roundingPower();
#endif
    }

    HANDLE         g_handle = nullptr;
    // после каждой перезагрузки конфига отдаём ядру настройки сразу, а не на первом
    // кадре скриншера: обложка по умолчанию успевает прогреться до захвата
    CHyprSignalListener g_onReload;
    CFunctionHook* g_hook   = nullptr;

    SP<Config::Values::CStringValue> g_cfgPath;
    SP<Config::Values::CBoolValue>   g_cfgLoop;
    SP<Config::Values::CFloatValue>  g_cfgSpeed;
    SP<Config::Values::CStringValue> g_cfgBackend;
    SP<Config::Values::CStringValue> g_cfgGpu;

    using EffectId = Desktop::Rule::CWindowRuleEffectContainer::storageType;

    // Эффекты правил. Lua-конфиг upstream передаёт плагинам только плоские поля
    // (string/bool/number, таблицы отвергает), поэтому основные имена — обычные
    // Lua-идентификаторы, без обёрток над hl.window_rule:
    //   hl.window_rule({ match = {...}, no_screen_share = true,
    //                    no_screen_share_cover = "~/x.mp4", no_screen_share_cover_speed = 1.5 })
    // Имена с двоеточием — из исходного плагина, чтобы старые конфиги не ломались.
    enum eField : uint8_t { FIELD_PATH, FIELD_SPEED, FIELD_LOOP };
    struct SEffect {
        const char* name;
        eField      field;
        EffectId    id = 0;
    };
    std::array<SEffect, 6> g_effects = {{
        {"no_screen_share_cover", FIELD_PATH},
        {"no_screen_share_cover_speed", FIELD_SPEED},
        {"no_screen_share_cover_loop", FIELD_LOOP},
        {"no_screen_share_cover:path_cover", FIELD_PATH},
        {"no_screen_share_cover:speed", FIELD_SPEED},
        {"no_screen_share_cover:loop", FIELD_LOOP},
    }};

    // Кэш текстур по id обложки. Сбрасывается целиком при смене эпохи ядра.
    struct SCachedTexture {
        SP<Render::ITexture> tex;
        uint64_t             generation = 0;
        uint32_t             w = 0, h = 0;
        bool                 dmabuf = false;
    };
    std::unordered_map<uint64_t, SCachedTexture> g_textures;
    uint64_t                                     g_epoch      = 0;
    uint64_t                                     g_frameCount = 0;

    void notify(const std::string& msg, float time = 4000) {
        if (g_handle)
            HyprlandAPI::addNotification(g_handle, msg, CHyprColor{1.F, 0.2F, 0.2F, 1.F}, time);
    }

    // NOSHARE_COVER_DEBUG=<файл> — трейс по шагам (для отладки на чужой машине)
    FILE* const g_trace = [] {
        const char* v = std::getenv("NOSHARE_COVER_DEBUG");
        return v && *v ? std::fopen(v, "a") : nullptr;
    }();
#define NSC_TRACE(...) \
    do { \
        if (g_trace) { \
            std::fprintf(g_trace, "[noshare-cover] " __VA_ARGS__); \
            std::fflush(g_trace); \
        } \
    } while (0)

    void drainNotifications() {
        std::array<char, 512> buf{};
        while (nsc_take_notification(buf.data(), buf.size()) > 0)
            notify(buf.data());
    }

    void pushSettings() {
        const std::string path    = g_cfgPath ? g_cfgPath->value() : std::string{};
        const std::string backend = g_cfgBackend ? g_cfgBackend->value() : std::string{"auto"};
        const std::string gpu     = g_cfgGpu ? g_cfgGpu->value() : std::string{};

        const nsc_settings s{
            .path_cover = path.c_str(),
            .loop       = g_cfgLoop ? g_cfgLoop->value() : true,
            .speed      = g_cfgSpeed ? static_cast<double>(g_cfgSpeed->value()) : 1.0,
            .backend    = backend.c_str(),
            .gpu_device = gpu.c_str(),
        };
        nsc_set_settings(&s); // ядро само сравнит с прошлыми и сбросит обложки только при изменении

        if (const auto epoch = nsc_epoch(); epoch != g_epoch) {
            g_textures.clear();
            g_epoch = epoch;
        }
    }

    // Кадр ядра -> текстура Hyprland. Новая текстура — только когда сменился кадр.
    SP<Render::ITexture> textureFor(const nsc_frame& f) {
        auto& c = g_textures[f.cover_id];
        if (c.tex && c.generation == f.generation)
            return c.tex;

        if (f.kind == NSC_FRAME_CPU && f.pixels) {
            auto* px = const_cast<uint8_t*>(f.pixels);
            if (c.tex && !c.dmabuf && c.w == f.width && c.h == f.height && c.tex->ok()) {
                const CRegion damage{0.0, 0.0, double(f.width), double(f.height)};
                c.tex->update(f.fourcc, px, f.stride, damage);
            } else {
                c.tex = g_pHyprRenderer->createTexture(f.fourcc, px, f.stride, Vector2D{double(f.width), double(f.height)});
            }
            c.dmabuf = false;
        } else if (f.kind == NSC_FRAME_DMABUF && f.plane_count > 0 && f.plane_count <= 4) {
            Aquamarine::SDMABUFAttrs attrs;
            attrs.success  = true;
            attrs.size     = Vector2D{double(f.width), double(f.height)};
            attrs.format   = f.fourcc;
            attrs.modifier = f.modifier;
            attrs.planes   = int(f.plane_count);
            for (uint32_t i = 0; i < f.plane_count; ++i) {
                attrs.fds[i]     = f.planes[i].fd;
                attrs.offsets[i] = f.planes[i].offset;
                attrs.strides[i] = f.planes[i].stride;
            }
            c.tex    = g_pHyprRenderer->createTexture(attrs);
            c.dmabuf = true;
        } else {
            return nullptr;
        }

        c.w          = f.width;
        c.h          = f.height;
        c.generation = f.generation;
        return (c.tex && c.tex->ok()) ? c.tex : nullptr;
    }

    // Раз в пару секунд выкидываем текстуры обложек, которых ядро уже закрыло.
    void pruneTextures() {
        if (++g_frameCount % 120 != 0)
            return;
        std::erase_if(g_textures, [](const auto& kv) { return !nsc_cover_alive(kv.first); });
    }

    // Последнее совпавшее правило побеждает — как у самого Hyprland.
    struct SRuleValues {
        std::optional<std::string> path, speed, loop;
    };

    SRuleValues ruleValuesFor(const PHLWINDOW& w) {
        SRuleValues out;
        const auto& engine = Desktop::Rule::ruleEngine();
        if (!engine)
            return out;
        for (const auto& rule : engine->rules()) {
            if (!rule || rule->type() != Desktop::Rule::RULE_TYPE_WINDOW)
                continue;
            const auto winRule = dynamicPointerCast<Desktop::Rule::CWindowRule>(rule);
            if (!winRule || !winRule->matches(w))
                continue;
            for (const auto& effect : winRule->effects()) {
                if (effect.raw.empty())
                    continue;
                for (const auto& fx : g_effects) {
                    if (!fx.id || effect.key != fx.id)
                        continue;
                    switch (fx.field) {
                        case FIELD_PATH: out.path = effect.raw; break;
                        case FIELD_SPEED: out.speed = effect.raw; break;
                        case FIELD_LOOP: out.loop = effect.raw; break;
                    }
                }
            }
        }
        return out;
    }

    // Обложка окна для прямоугольника другого плагина: те же правила, что и у самого окна.
    SP<Render::ITexture> coverForWindowAddress(uint64_t address) {
        if (!address)
            return nullptr;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || reinterpret_cast<uintptr_t>(w.get()) != address)
                continue;
            const auto             rules = ruleValuesFor(w);
            const nsc_play_request req{
                .rule_path  = rules.path ? rules.path->c_str() : nullptr,
                .rule_speed = rules.speed ? rules.speed->c_str() : nullptr,
                .rule_loop  = rules.loop ? rules.loop->c_str() : nullptr,
            };
            nsc_frame f{};
            return nsc_resolve(&req, &f) ? textureFor(f) : nullptr;
        }
        return nullptr;
    }

    void paintExtraRects(const PHLMONITOR& mon, const Vector2D& capturePos) {
        // Обычно это десятки плиток оверлея; если больше — берём столько, сколько отдали.
        std::vector<nsc_extra_rect> rects(64);
        size_t                      n = nsc_extra_rects(mon->m_id, rects.data(), rects.size());
        if (n > rects.size()) {
            rects.resize(n);
            n = std::min(nsc_extra_rects(mon->m_id, rects.data(), rects.size()), rects.size());
        }

        for (size_t i = 0; i < n; ++i) {
            const auto& r     = rects[i];
            const auto  box   = CBox{r.x, r.y, std::max(r.w, 1.0), std::max(r.h, 1.0)}.translate(-mon->m_position).scale(mon->m_scale).translate(-capturePos);
            const int   round = int(std::lround(r.rounding * mon->m_scale));
            if (box.w < 1 || box.h < 1)
                continue;

            if (r.fill == 1) {
                if (const auto tex = coverForWindowAddress(r.window)) {
                    g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = tex, .box = box, .round = round}, box);
                    continue;
                }
            }
            g_pHyprRenderer->draw(
                CRectPassElement::SRectData{
                    .box   = box,
                    .color = CHyprColor{0.F, 0.F, 0.F, 1.F},
                    .round = round,
                },
                box);
        }
    }

    void paintCovers(Screenshare::CScreenshareFrame* frame) {
        if (!frame || !frame->m_session || !g_pHyprRenderer) {
            NSC_TRACE("skip: frame %p session %d renderer %d\n", static_cast<void*>(frame), frame && frame->m_session ? 1 : 0, g_pHyprRenderer ? 1 : 0);
            return;
        }
        const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!mon) {
            NSC_TRACE("skip: no monitor in render data\n");
            return;
        }

        pushSettings();
        nsc_begin_frame();
        NSC_TRACE("frame: monitor %s\n", mon->m_name.c_str());

        const auto capturePos = frame->m_session->m_captureBox.pos();

        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || !w->m_ruleApplicator || !w->m_ruleApplicator->noScreenShare().valueOrDefault())
                continue;
            if (!g_pHyprRenderer->shouldRenderWindow(w, mon) || w->isHidden()) {
                NSC_TRACE("window %s: not rendered on this monitor\n", w->m_class.c_str());
                continue;
            }

            const auto  fade = windowFade(w);
            const auto* ws   = w->m_workspace.get();
            if (!ws && fade != 0.F)
                continue;

            const bool pinned       = windowPinned(w);
            const auto renderOffset = ws && !pinned && ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
            const auto size         = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const auto pos          = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + renderOffset;
            const auto box          = CBox{pos.x, pos.y, std::max(size.x, 5.0), std::max(size.y, 5.0)}.translate(-mon->m_position).scale(mon->m_scale).translate(-capturePos);
            if (box.w < 1 || box.h < 1)
                continue;

            const auto             rules = ruleValuesFor(w);
            const nsc_play_request req{
                .rule_path  = rules.path ? rules.path->c_str() : nullptr,
                .rule_speed = rules.speed ? rules.speed->c_str() : nullptr,
                .rule_loop  = rules.loop ? rules.loop->c_str() : nullptr,
            };
            nsc_frame f{};
            if (!nsc_resolve(&req, &f)) {
                NSC_TRACE("window %s: no cover frame yet\n", w->m_class.c_str());
                continue;
            }
            const auto tex = textureFor(f);
            if (!tex) {
                NSC_TRACE("window %s: texture failed (%ux%u)\n", w->m_class.c_str(), f.width, f.height);
                continue;
            }
            NSC_TRACE("window %s: cover %ux%u at %.0f,%.0f %.0fx%.0f\n", w->m_class.c_str(), f.width, f.height, box.x, box.y, box.w, box.h);

            const bool fullscreen = Fullscreen::controller() && Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN);
            const bool dontRound  = capturePos != Vector2D{} || fullscreen;
            g_pHyprRenderer->draw(
                CTexPassElement::SRenderData{
                    .tex           = tex,
                    .box           = box,
                    .round         = dontRound ? 0 : int(std::lround(windowRounding(w) * mon->m_scale)),
                    .roundingPower = dontRound ? 2.F : windowRoundingPower(w),
                },
                box);
        }

        paintExtraRects(mon, capturePos);
        nsc_end_frame();
        pruneTextures();
        drainNotifications();
    }

    using RenderMonitorFn = void (*)(Screenshare::CScreenshareFrame*);

    void hkRenderMonitor(Screenshare::CScreenshareFrame* self) {
        NSC_TRACE("hook: renderMonitor\n");
        reinterpret_cast<RenderMonitorFn>(g_hook->m_original)(self);
        paintCovers(self);
    }

    template <typename T, typename... Args>
    SP<T> makeValue(const char* name, const char* desc, Args&&... def) {
        auto v = Config::Values::makeConfigValue<T>(name, desc, std::forward<Args>(def)...);
        if (!v || !HyprlandAPI::addConfigValueV2(g_handle, v))
            notify(std::string("noshare-cover: не встал параметр ") + name, 5000);
        return v;
    }

} // namespace

// Публичный ABI для других плагинов (include/noshare_cover_api.h). Rust-архив
// слинкован скрытым, поэтому наружу имена выставляем здесь, тонкими обёртками.
#define NSC_PUBLIC extern "C" __attribute__((visibility("default")))
NSC_PUBLIC uint32_t noshare_cover_api_version() {
    return nsc_api_api_version();
}
NSC_PUBLIC uint64_t noshare_cover_register_client(const char* name) {
    return nsc_api_register_client(name);
}
NSC_PUBLIC void noshare_cover_unregister_client(uint64_t client) {
    nsc_api_unregister_client(client);
}
NSC_PUBLIC bool noshare_cover_set_rects(uint64_t client, int monitor_id, const noshare_cover_rect* rects, size_t count) {
    return nsc_api_set_rects(client, monitor_id, rects, count);
}
NSC_PUBLIC bool noshare_cover_clear_client_rects(uint64_t client) {
    return nsc_api_clear_client_rects(client);
}
NSC_PUBLIC void noshare_cover_clear_extra_rects() {
    nsc_api_clear_extra_rects();
}
NSC_PUBLIC void noshare_cover_add_extra_rect(int monitor_id, double x, double y, double w, double h, double rounding) {
    nsc_api_add_extra_rect(monitor_id, x, y, w, h, rounding);
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;
    const PLUGIN_DESCRIPTION_INFO info{"noshare-cover", "image or video instead of the no_screen_share black box", "vlad", "0.2"};

    // Плагин, собранный под другие заголовки, лезет в чужие смещения полей и
    // роняет композитор. Отказываемся сразу: Hyprland поймает исключение,
    // выгрузит плагин и покажет причину.
    if (std::string{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash()) {
        notify("noshare-cover: собран под другую версию Hyprland, пересоберите (hyprpm update)", 10000);
        throw std::runtime_error("noshare-cover: Hyprland version mismatch");
    }

    if (!nsc_init())
        throw std::runtime_error("noshare-cover: core init failed");

    for (auto& fx : g_effects)
        fx.id = Desktop::Rule::windowEffects()->registerEffect(fx.name);

    g_cfgPath    = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:path_cover", "Default media for no_screen_share windows", Config::STRING{});
    g_cfgLoop    = makeValue<Config::Values::CBoolValue>("plugin:no_screen_share_cover:loop", "Loop gif and video", true);
    g_cfgSpeed   = makeValue<Config::Values::CFloatValue>("plugin:no_screen_share_cover:speed", "Playback speed for gif and video", 1.F);
    g_cfgBackend = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:backend", "Video decode backend: auto, gpu or cpu", Config::STRING{"auto"});
    g_cfgGpu     = makeValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:gpu_device", "Render node for GPU decode, empty = first one", Config::STRING{});

    void* target = nullptr;
    for (const auto& match : HyprlandAPI::findFunctionsByName(handle, "renderMonitor")) {
        if (match.demangled.find("CScreenshareFrame::renderMonitor") != std::string::npos) {
            target = match.address;
            break;
        }
    }
    if (!target)
        throw std::runtime_error("noshare-cover: CScreenshareFrame::renderMonitor not found");

    g_onReload = Event::bus()->m_events.config.reloaded.listen([] {
        pushSettings();
        drainNotifications();
    });

    g_hook = HyprlandAPI::createFunctionHook(handle, target, reinterpret_cast<void*>(&hkRenderMonitor));
    if (!g_hook || !g_hook->hook())
        throw std::runtime_error("noshare-cover: failed to hook renderMonitor");
    return info;
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_onReload.reset();
    // Сначала снимаем хук: Hyprland чистит хуки уже после PLUGIN_EXIT, и кадр
    // скриншера между этим не должен попасть в выгруженное ядро.
    if (g_hook) {
        HyprlandAPI::removeFunctionHook(g_handle, g_hook);
        g_hook = nullptr;
    }
    g_textures.clear();
    nsc_shutdown(); // останавливает и join-ит потоки декода, чистит extra rects

    if (const auto& fx = Desktop::Rule::windowEffects()) {
        for (const auto& e : g_effects)
            if (e.id)
                fx->unregisterEffect(e.id);
    }
    for (auto& e : g_effects)
        e.id = 0;
    g_cfgPath.reset();
    g_cfgLoop.reset();
    g_cfgSpeed.reset();
    g_cfgBackend.reset();
    g_cfgGpu.reset();
    g_handle = nullptr;
}
