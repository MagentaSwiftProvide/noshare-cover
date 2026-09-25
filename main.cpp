#include <drm_fourcc.h>
#include <gif_lib.h>
#include <cstdio>
#include <jpeglib.h>
#include <setjmp.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <any>
#include <cairo/cairo.h>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <unistd.h>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define private public
#include "managers/screenshare/ScreenshareManager.hpp"
#undef private

#include "desktop/rule/Engine.hpp"
#include "desktop/rule/windowRule/WindowRule.hpp"
#include "desktop/state/WindowState.hpp"
#include "desktop/view/Window.hpp"
#include "managers/fullscreen/FullscreenController.hpp"
#include "config/values/ConfigValues.hpp"
#include "plugins/PluginAPI.hpp"
#include "render/pass/TexPassElement.hpp"

static HANDLE        PHANDLE = nullptr;
static CFunctionHook* g_hook  = nullptr;

enum class eKind { None, Still, Gif, Video };
static std::string g_error;

static void notifyOnce(const std::string& msg) {
    if (msg == g_error || !PHANDLE)
        return;
    g_error = msg;
    HyprlandAPI::addNotification(PHANDLE, msg, CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 4000);
}

struct SConf {
    std::string file;
    bool        loop  = true;
    double      speed = 1.0;
    bool        ready = false;
} g_conf;

static SP<Config::Values::CStringValue> g_cfgFile;
static SP<Config::Values::CBoolValue>   g_cfgLoop;
static SP<Config::Values::CFloatValue>  g_cfgSpeed;
static Desktop::Rule::CWindowRuleEffectContainer::storageType g_coverEffect = 0;
static Desktop::Rule::CWindowRuleEffectContainer::storageType g_speedEffect = 0;
static Desktop::Rule::CWindowRuleEffectContainer::storageType g_loopEffect  = 0;

struct SVideo {
    AVFormatContext* fmt    = nullptr;
    AVCodecContext*  dec    = nullptr;
    SwsContext*      sws    = nullptr;
    AVFrame*         frame  = nullptr;
    AVPacket*        pkt    = nullptr;
    int              stream = -1;
    int64_t          durationUs = 0;
    int64_t          lastUs     = -1;
    int64_t          t0         = 0;
    bool             draining   = false;
};

struct SGifFrame {
    int delayMs     = 100;
    int disposal    = 1;
    int transparent = -1;
    int left = 0, top = 0, width = 0, height = 0;
};

struct SGif {
    GifFileType*          file  = nullptr;
    int                   w     = 0;
    int                   h     = 0;
    int                   index = -1;
    int64_t               totalMs = 0;
    int64_t               t0      = 0;
    uint32_t              bg      = 0;
    std::vector<uint32_t> canvas;
    std::vector<uint32_t> backup;
    std::vector<SGifFrame> frames;
};

struct SCover {
    eKind                kind = eKind::None;
    SP<Render::ITexture> tex;
    SGif                 gif;
    SVideo               vid;
    double               speed   = 1.0;
    bool                 loop    = true;
    bool                 missing = false;
    std::string          error;
};

struct SPlay {
    std::string path;
    double      speed = 1.0;
    bool        loop  = true;

    std::string key() const {
        return path + "\n" + std::to_string(speed) + (loop ? "\n1" : "\n0");
    }
};

static SCover*                                                      active = nullptr;
static std::unordered_map<std::string, std::unique_ptr<SCover>> g_media;

static std::string homeDir() {
    if (const char* home = std::getenv("HOME"); home && *home)
        return home;
    if (const passwd* pw = getpwuid(geteuid()); pw && pw->pw_dir && *pw->pw_dir)
        return pw->pw_dir;
    return {};
}

static std::string configFile(const char* name) {
    return homeDir() + "/.config/hypr/" + name;
}

static std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string expandHome(std::string path) {
    if (path == "~")
        return homeDir();
    if (path.starts_with("~/"))
        return homeDir() + path.substr(1);
    return path;
}

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint32_t packPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint32_t R = (uint32_t(r) * a + 127) / 255;
    const uint32_t G = (uint32_t(g) * a + 127) / 255;
    const uint32_t B = (uint32_t(b) * a + 127) / 255;
    return (uint32_t(a) << 24) | (R << 16) | (G << 8) | B;
}

static int extensionValue(const SavedImage& image, int* transparent) {
    int delay    = 100;
    int disposal = 1;
    if (transparent)
        *transparent = -1;

    for (int i = 0; i < image.ExtensionBlockCount; ++i) {
        const auto& ext = image.ExtensionBlocks[i];
        if (ext.Function != GRAPHICS_EXT_FUNC_CODE || ext.ByteCount < 4 || !ext.Bytes)
            continue;
        disposal = (ext.Bytes[0] >> 2) & 0x7;
        const int cs = ext.Bytes[1] | (ext.Bytes[2] << 8);
        delay         = (cs <= 1 ? 10 : cs) * 10;
        if (transparent && (ext.Bytes[0] & 0x1))
            *transparent = static_cast<uint8_t>(ext.Bytes[3]);
    }
    return delay | (disposal << 16);
}

static void clearRect(int left, int top, int width, int height, uint32_t color) {
    const int x0 = std::max(left, 0);
    const int y0 = std::max(top, 0);
    const int x1 = std::min(left + width, active->gif.w);
    const int y1 = std::min(top + height, active->gif.h);
    for (int y = y0; y < y1; ++y) {
        auto* row = active->gif.canvas.data() + y * active->gif.w;
        std::fill(row + x0, row + x1, color);
    }
}

static void blitGifFrame(int index) {
    const auto& frame = active->gif.frames[index];
    const auto& image = active->gif.file->SavedImages[index];
    const auto* map   = image.ImageDesc.ColorMap ? image.ImageDesc.ColorMap : active->gif.file->SColorMap;
    if (!map || !image.RasterBits || frame.width <= 0 || frame.height <= 0)
        return;

    for (int y = 0; y < frame.height; ++y) {
        const int dy = frame.top + y;
        if (dy < 0 || dy >= active->gif.h)
            continue;
        for (int x = 0; x < frame.width; ++x) {
            const int dx = frame.left + x;
            if (dx < 0 || dx >= active->gif.w)
                continue;
            const int px = image.RasterBits[y * frame.width + x];
            if (px == frame.transparent || px >= map->ColorCount)
                continue;
            const auto& c = map->Colors[px];
            active->gif.canvas[dy * active->gif.w + dx] = packPixel(c.Red, c.Green, c.Blue, 255);
        }
    }
}

static void applyDisposal(int index) {
    const auto& frame = active->gif.frames[index];
    if (frame.disposal == 2)
        clearRect(frame.left, frame.top, frame.width, frame.height, active->gif.bg);
    else if (frame.disposal == 3 && active->gif.backup.size() == active->gif.canvas.size())
        active->gif.canvas = active->gif.backup;
}

static void stepGif() {
    const int count = static_cast<int>(active->gif.frames.size());
    const int next  = active->gif.index + 1;
    if (next < 0 || next >= count)
        return;
    if (active->gif.index >= 0)
        applyDisposal(active->gif.index);
    if (active->gif.frames[next].disposal == 3)
        active->gif.backup = active->gif.canvas;
    blitGifFrame(next);
    active->gif.index = next;
}

static void uploadCover() {
    if (!g_pHyprRenderer || active->gif.canvas.empty())
        return;

    if (!active->tex || !active->tex->m_texID || active->tex->m_size.x != active->gif.w || active->tex->m_size.y != active->gif.h) {
        auto* surf = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(active->gif.canvas.data()), CAIRO_FORMAT_ARGB32, active->gif.w, active->gif.h, active->gif.w * 4);
        if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            if (surf)
                cairo_surface_destroy(surf);
            return;
        }
        active->tex = g_pHyprRenderer->createTexture(surf);
        cairo_surface_destroy(surf);
        return;
    }

    const CRegion damage{0.0, 0.0, static_cast<double>(active->gif.w), static_cast<double>(active->gif.h)};
    active->tex->update(DRM_FORMAT_ARGB8888, reinterpret_cast<uint8_t*>(active->gif.canvas.data()), static_cast<uint32_t>(active->gif.w * 4), damage);
}

static int frameAt(int64_t pos) {
    int64_t acc = 0;
    for (int i = 0; i < static_cast<int>(active->gif.frames.size()); ++i) {
        acc += active->gif.frames[i].delayMs;
        if (pos < acc)
            return i;
    }
    return static_cast<int>(active->gif.frames.size()) - 1;
}

static void showGifFrame(int target) {
    if (target == active->gif.index)
        return;
    if (target < active->gif.index) {
        std::fill(active->gif.canvas.begin(), active->gif.canvas.end(), active->gif.bg);
        active->gif.index = -1;
    }
    int guard = 0;
    while (active->gif.index != target && guard++ < static_cast<int>(active->gif.frames.size()) + 2)
        stepGif();
    uploadCover();
}

static void syncGif() {
    if (!active->gif.file || active->gif.frames.empty() || active->gif.totalMs < 1)
        return;
    const auto now = nowMs();
    if (active->gif.t0 == 0)
        active->gif.t0 = now;
    const double speed = active->speed > 0.0 ? active->speed : 1.0;
    auto         elapsed = static_cast<int64_t>((now - active->gif.t0) * speed);
    if (elapsed < 0)
        elapsed = 0;
    if (!active->loop)
        elapsed = std::min(elapsed, active->gif.totalMs - 1);
    else
        elapsed %= active->gif.totalMs;
    showGifFrame(frameAt(elapsed));
}

static bool loadGif(const std::string& path) {
    int  err = 0;
    auto* file = DGifOpenFileName(path.c_str(), &err);
    if (!file || DGifSlurp(file) != GIF_OK || file->ImageCount < 1 || file->SWidth < 1 || file->SHeight < 1) {
        if (file)
            DGifCloseFile(file, &err);
        return false;
    }

    active->gif.file = file;
    active->gif.w    = file->SWidth;
    active->gif.h    = file->SHeight;
    active->gif.canvas.assign(static_cast<size_t>(active->gif.w) * active->gif.h, 0);
    active->gif.frames.resize(file->ImageCount);

    if (file->SColorMap && file->SBackGroundColor >= 0 && file->SBackGroundColor < file->SColorMap->ColorCount) {
        const auto& c = file->SColorMap->Colors[file->SBackGroundColor];
        active->gif.bg      = packPixel(c.Red, c.Green, c.Blue, 255);
    }
    std::fill(active->gif.canvas.begin(), active->gif.canvas.end(), active->gif.bg);

    for (int i = 0; i < file->ImageCount; ++i) {
        const auto& image = file->SavedImages[i];
        int         transparent = -1;
        const int   packed      = extensionValue(image, &transparent);
        auto&       frame       = active->gif.frames[i];
        frame.delayMs           = packed & 0xffff;
        frame.disposal          = (packed >> 16) & 0x7;
        frame.transparent       = transparent;
        frame.left              = image.ImageDesc.Left;
        frame.top               = image.ImageDesc.Top;
        frame.width             = image.ImageDesc.Width;
        frame.height            = image.ImageDesc.Height;
        active->gif.totalMs += frame.delayMs;
    }

    showGifFrame(0);
    active->kind = eKind::Gif;
    return active->tex && active->tex->ok() && active->tex->m_texID;
}

static bool loadPng(const std::string& path) {
    auto* surf = cairo_image_surface_create_from_png(path.c_str());
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS || cairo_image_surface_get_width(surf) <= 0) {
        if (surf)
            cairo_surface_destroy(surf);
        return false;
    }

    active->tex = g_pHyprRenderer->createTexture(surf);
    cairo_surface_destroy(surf);
    active->kind  = eKind::Still;
    return active->tex && active->tex->ok() && active->tex->m_texID;
}

struct SJpegErr {
    jpeg_error_mgr pub;
    jmp_buf        jmp;
};

static void jpegBail(j_common_ptr cinfo) {
    longjmp(reinterpret_cast<SJpegErr*>(cinfo->err)->jmp, 1);
}

static bool loadJpeg(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;

    jpeg_decompress_struct cinfo{};
    SJpegErr               jerr{};
    cinfo.err       = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegBail;
    if (setjmp(jerr.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(file);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, file);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    active->gif.w = static_cast<int>(cinfo.output_width);
    active->gif.h = static_cast<int>(cinfo.output_height);
    if (active->gif.w < 1 || active->gif.h < 1) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(file);
        return false;
    }

    active->gif.canvas.assign(static_cast<size_t>(active->gif.w) * active->gif.h, 0);
    std::vector<uint8_t> row(static_cast<size_t>(active->gif.w) * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* scan = row.data();
        jpeg_read_scanlines(&cinfo, &scan, 1);
        const int y = static_cast<int>(cinfo.output_scanline) - 1;
        for (int x = 0; x < active->gif.w; ++x)
            active->gif.canvas[y * active->gif.w + x] = packPixel(row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 255);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(file);
    uploadCover();
    active->kind = eKind::Still;
    return active->tex && active->tex->ok() && active->tex->m_texID;
}

static void closeVideo() {
    if (active->vid.pkt)
        av_packet_free(&active->vid.pkt);
    if (active->vid.frame)
        av_frame_free(&active->vid.frame);
    if (active->vid.sws)
        sws_freeContext(active->vid.sws);
    if (active->vid.dec)
        avcodec_free_context(&active->vid.dec);
    if (active->vid.fmt)
        avformat_close_input(&active->vid.fmt);
    active->vid = {};
}

static bool presentVideoFrame() {
    if (!active->vid.frame || active->vid.frame->width < 1 || active->vid.frame->height < 1)
        return false;

    active->vid.sws = sws_getCachedContext(active->vid.sws, active->vid.frame->width, active->vid.frame->height, static_cast<AVPixelFormat>(active->vid.frame->format), active->gif.w, active->gif.h, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!active->vid.sws)
        return false;

    uint8_t* dstData[4]  = {reinterpret_cast<uint8_t*>(active->gif.canvas.data()), nullptr, nullptr, nullptr};
    int      dstStride[4] = {active->gif.w * 4, 0, 0, 0};
    sws_scale(active->vid.sws, active->vid.frame->data, active->vid.frame->linesize, 0, active->vid.frame->height, dstData, dstStride);
    uploadCover();
    return active->tex && active->tex->ok() && active->tex->m_texID;
}

static bool pullVideoFrame() {
    auto* st = active->vid.fmt->streams[active->vid.stream];
    for (int spins = 0; spins < 256; ++spins) {
        const int got = avcodec_receive_frame(active->vid.dec, active->vid.frame);
        if (got == 0) {
            const int64_t pts = active->vid.frame->best_effort_timestamp != AV_NOPTS_VALUE ? active->vid.frame->best_effort_timestamp : active->vid.frame->pts;
            active->vid.lastUs      = pts == AV_NOPTS_VALUE ? active->vid.lastUs + 1 : av_rescale_q(pts, st->time_base, AV_TIME_BASE_Q);
            return true;
        }
        if (got != AVERROR(EAGAIN) && got != AVERROR_EOF)
            return false;

        if (active->vid.draining)
            return false;

        if (av_read_frame(active->vid.fmt, active->vid.pkt) < 0) {
            avcodec_send_packet(active->vid.dec, nullptr);
            active->vid.draining = true;
            continue;
        }
        if (active->vid.pkt->stream_index == active->vid.stream)
            avcodec_send_packet(active->vid.dec, active->vid.pkt);
        av_packet_unref(active->vid.pkt);
    }
    return false;
}

static void seekVideo(int64_t elapsedUs) {
    auto* st = active->vid.fmt->streams[active->vid.stream];
    const auto pts = av_rescale_q(elapsedUs, AV_TIME_BASE_Q, st->time_base);
    av_seek_frame(active->vid.fmt, active->vid.stream, std::max<int64_t>(pts, 0), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(active->vid.dec);
    active->vid.lastUs   = -1;
    active->vid.draining = false;
}

static bool loadVideo(const std::string& path) {
    closeVideo();
    if (avformat_open_input(&active->vid.fmt, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(active->vid.fmt, nullptr) < 0)
        return false;

    const AVCodec* codec = nullptr;
    active->vid.stream         = av_find_best_stream(active->vid.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (active->vid.stream < 0 || !codec)
        return false;

    active->vid.dec = avcodec_alloc_context3(codec);
    if (!active->vid.dec || avcodec_parameters_to_context(active->vid.dec, active->vid.fmt->streams[active->vid.stream]->codecpar) < 0 || avcodec_open2(active->vid.dec, codec, nullptr) < 0)
        return false;

    active->vid.frame = av_frame_alloc();
    active->vid.pkt   = av_packet_alloc();
    active->gif.w     = active->vid.dec->width;
    active->gif.h     = active->vid.dec->height;
    if (!active->vid.frame || !active->vid.pkt || active->gif.w < 1 || active->gif.h < 1)
        return false;

    auto* st = active->vid.fmt->streams[active->vid.stream];
    if (st->duration > 0)
        active->vid.durationUs = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
    else if (active->vid.fmt->duration > 0)
        active->vid.durationUs = active->vid.fmt->duration;

    active->gif.canvas.assign(static_cast<size_t>(active->gif.w) * active->gif.h, packPixel(0, 0, 0, 255));
    if (!pullVideoFrame() || !presentVideoFrame())
        return false;

    active->kind = eKind::Video;
    return true;
}

static void syncVideo() {
    if (active->kind != eKind::Video || !active->vid.fmt || active->vid.stream < 0)
        return;

    const auto now = nowMs();
    if (active->vid.t0 == 0)
        active->vid.t0 = now;

    const double speed = active->speed > 0.0 ? active->speed : 1.0;
    int64_t      elapsedUs = static_cast<int64_t>((now - active->vid.t0) * speed * 1000.0);
    if (elapsedUs < 0)
        elapsedUs = 0;
    if (active->vid.durationUs > 0) {
        if (active->loop)
            elapsedUs %= active->vid.durationUs;
        else
            elapsedUs = std::min(elapsedUs, active->vid.durationUs);
    }

    if (active->vid.lastUs < 0 || elapsedUs + 50000 < active->vid.lastUs)
        seekVideo(elapsedUs);

    bool presented = false;
    int  guard     = 0;
    while (active->vid.lastUs < elapsedUs && guard++ < 48) {
        if (!pullVideoFrame()) {
            if (active->loop && active->vid.durationUs > 0) {
                seekVideo(0);
                active->vid.t0 = now;
            }
            break;
        }
        presented = true;
    }
    if (presented)
        presentVideoFrame();
}

static void closeGif() {
    if (!active->gif.file)
        return;
    int err = 0;
    DGifCloseFile(active->gif.file, &err);
    active->gif.file = nullptr;
    active->gif.frames.clear();
    active->gif.index   = -1;
    active->gif.totalMs = 0;
    active->gif.t0      = 0;
}

static void unloadMedia() {
    closeGif();
    closeVideo();
    active->gif.canvas.clear();
    active->kind = eKind::None;
}

static std::string defaultMediaPath() {
    for (const char* name : {"noshare-cover.gif", "noshare-cover.jpg", "noshare-cover.jpeg", "noshare-cover.png", "noshare-cover.mp4"}) {
        if (std::filesystem::exists(configFile(name)))
            return "~/.config/hypr/" + std::string{name};
    }
    return "~/.config/hypr/noshare-cover.gif";
}

static bool readConfig() {
    if (!g_cfgFile || !g_cfgLoop || !g_cfgSpeed)
        return false;

    const auto file    = expandHome(g_cfgFile->value().empty() ? defaultMediaPath() : g_cfgFile->value());
    const bool loop    = g_cfgLoop->value();
    const auto speed   = static_cast<double>(g_cfgSpeed->value());
    const bool changed = !g_conf.ready || file != g_conf.file || loop != g_conf.loop || speed != g_conf.speed;
    if (!changed)
        return false;

    g_conf.file  = file;
    g_conf.loop  = loop;
    g_conf.speed = speed;
    g_conf.ready = true;
    return true;
}

static void dropMedia() {
    for (auto& [_, cover] : g_media) {
        active = cover.get();
        unloadMedia();
        cover->tex.reset();
    }
    g_media.clear();
    active = nullptr;
}

static bool loadMedia(const std::string& path) {
    if (!active)
        return false;
    if (path.empty() || !std::filesystem::exists(path)) {
        active->missing = true;
        notifyOnce("noshare-cover: нет файла " + path);
        return false;
    }

    const auto ext = lower(std::filesystem::path(path).extension().string());
    bool       ok  = false;
    if (ext == ".gif")
        ok = loadGif(path);
    else if (ext == ".png")
        ok = loadPng(path);
    else if (ext == ".jpg" || ext == ".jpeg")
        ok = loadJpeg(path);
    else if (ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".webm" || ext == ".mkv")
        ok = loadVideo(path);
    else {
        active->error = ext;
        notifyOnce("noshare-cover: не знаю формат " + ext);
        return false;
    }

    if (!ok) {
        unloadMedia();
        active->error = path;
        notifyOnce("noshare-cover: не открылся " + path);
        return false;
    }
    active->error.clear();
    active->missing = false;
    return true;
}

static SCover* ensurePath(const SPlay& play) {
    const auto key = play.key();
    auto       it  = g_media.find(key);
    if (it == g_media.end()) {
        auto cover   = std::make_unique<SCover>();
        cover->speed = play.speed;
        cover->loop  = play.loop;
        it           = g_media.emplace(key, std::move(cover)).first;
    }

    auto* cover = it->second.get();
    active      = cover;
    if (cover->kind != eKind::None)
        return (cover->tex && cover->tex->ok() && cover->tex->m_texID) ? cover : nullptr;
    if (cover->missing) {
        std::error_code ec;
        if (play.path.empty() || !std::filesystem::exists(play.path, ec))
            return nullptr;
        cover->missing = false;
    } else if (!cover->error.empty())
        return nullptr;
    if (!loadMedia(play.path))
        return nullptr;
    return cover;
}

static bool coverTruthy(const std::string& raw) {
    const auto val = lower(raw);
    return !(val == "0" || val == "false" || val == "no" || val == "off");
}

static SPlay playFor(const PHLWINDOW& w) {
    SPlay play{g_conf.file, g_conf.speed > 0.0 ? g_conf.speed : 1.0, g_conf.loop};
    if (!Desktop::Rule::ruleEngine())
        return play;

    for (const auto& rule : Desktop::Rule::ruleEngine()->rules()) {
        if (!rule || rule->type() != Desktop::Rule::RULE_TYPE_WINDOW)
            continue;
        auto winRule = dynamicPointerCast<Desktop::Rule::CWindowRule>(rule);
        if (!winRule || !winRule->matches(w))
            continue;
        for (const auto& effect : winRule->effects()) {
            if (effect.raw.empty())
                continue;
            if (effect.key == g_coverEffect)
                play.path = expandHome(effect.raw);
            else if (effect.key == g_speedEffect) {
                try {
                    play.speed = std::stod(effect.raw);
                } catch (...) {
                    play.speed = 1.0;
                }
                if (play.speed <= 0.0)
                    play.speed = 1.0;
            } else if (effect.key == g_loopEffect)
                play.loop = coverTruthy(effect.raw);
        }
    }
    return play;
}

using RenderMonitorFn = void (*)(Screenshare::CScreenshareFrame*);

static void paintCovers(Screenshare::CScreenshareFrame* self) {
    if (!self || !self->m_session || !g_pHyprRenderer)
        return;
    if (readConfig()) {
        dropMedia();
        g_error.clear();
    }

    const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!mon)
        return;

    const auto                 capturePos = self->m_session->m_captureBox.pos();
    std::unordered_set<std::string> synced;

    for (const auto& w : Desktop::windowState()->windows()) {
        if (!w || !w->m_ruleApplicator || !w->m_ruleApplicator->noScreenShare().valueOrDefault())
            continue;
        if (!g_pHyprRenderer->shouldRenderWindow(w, mon) || w->isHidden())
            continue;

        const auto* ws = w->m_workspace.get();
        if (!ws && w->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN) != 0.F)
            continue;

        const auto renderOffset = ws && !w->m_pinned && ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
        const auto realSize     = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        const auto realPos      = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + renderOffset;
        const auto windowBox    = CBox{realPos.x, realPos.y, std::max(realSize.x, 5.0), std::max(realSize.y, 5.0)}
                                   .translate(-mon->m_position)
                                   .scale(mon->m_scale)
                                   .translate(-capturePos);

        if (windowBox.w < 1 || windowBox.h < 1)
            continue;

        const auto play  = playFor(w);
        auto*      cover = ensurePath(play);
        if (!cover || !cover->tex || !cover->tex->ok() || !cover->tex->m_texID)
            continue;
        if (synced.insert(play.key()).second) {
            active = cover;
            if (cover->kind == eKind::Gif)
                syncGif();
            else if (cover->kind == eKind::Video)
                syncVideo();
        }

        const bool dontRound = capturePos != Vector2D{} || (Fullscreen::controller() && Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN));
        const int  rounding  = dontRound ? 0 : static_cast<int>(std::lround(w->rounding() * mon->m_scale));
        const auto roundPow  = dontRound ? 2.F : w->roundingPower();

        g_pHyprRenderer->draw(CTexPassElement::SRenderData{
                                  .tex           = cover->tex,
                                  .box           = windowBox,
                                  .round         = rounding,
                                  .roundingPower = roundPow,
                              },
                              windowBox);
    }
}

static void hkRenderMonitor(Screenshare::CScreenshareFrame* self) {
    reinterpret_cast<RenderMonitorFn>(g_hook->m_original)(self);
    paintCovers(self);
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const auto matches = HyprlandAPI::findFunctionsByName(handle, "renderMonitor");
    void*      addr    = nullptr;
    for (const auto& match : matches) {
        if (match.demangled.find("CScreenshareFrame::renderMonitor") != std::string::npos) {
            addr = match.address;
            break;
        }
    }

    g_coverEffect = Desktop::Rule::windowEffects()->registerEffect("no_share_cover");
    g_speedEffect = Desktop::Rule::windowEffects()->registerEffect("no_share_cover_speed");
    g_loopEffect  = Desktop::Rule::windowEffects()->registerEffect("no_share_cover_loop");

    g_cfgFile  = Config::Values::makeConfigValue<Config::Values::CStringValue>("plugin:no_share_cover", "Default media for no_screen_share windows", Config::STRING{});
    g_cfgLoop  = Config::Values::makeConfigValue<Config::Values::CBoolValue>("plugin:no_share_cover_loop", "Loop gif and video", true);
    g_cfgSpeed = Config::Values::makeConfigValue<Config::Values::CFloatValue>("plugin:no_share_cover_speed", "Playback speed for gif and video", 1.F);
    if (!g_coverEffect || !g_speedEffect || !g_loopEffect || !g_cfgFile || !g_cfgLoop || !g_cfgSpeed || !HyprlandAPI::addConfigValueV2(handle, g_cfgFile) ||
        !HyprlandAPI::addConfigValueV2(handle, g_cfgLoop) || !HyprlandAPI::addConfigValueV2(handle, g_cfgSpeed))
        HyprlandAPI::addNotification(handle, "noshare-cover: конфиг не встал", CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 5000);

    if (!addr) {
        HyprlandAPI::addNotification(handle, "noshare-cover: не нашёл renderMonitor", CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 5000);
        return {"noshare-cover", "missing symbol", "vlad", "0.1"};
    }

    g_hook = HyprlandAPI::createFunctionHook(handle, addr, reinterpret_cast<void*>(&hkRenderMonitor));
    if (!g_hook || !g_hook->hook()) {
        HyprlandAPI::addNotification(handle, "noshare-cover: хук не встал", CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 5000);
        return {"noshare-cover", "hook failed", "vlad", "0.1"};
    }

    return {"noshare-cover", "image instead of no_screen_share black box", "vlad", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    dropMedia();
    if (Desktop::Rule::windowEffects()) {
        if (g_coverEffect)
            Desktop::Rule::windowEffects()->unregisterEffect(g_coverEffect);
        if (g_speedEffect)
            Desktop::Rule::windowEffects()->unregisterEffect(g_speedEffect);
        if (g_loopEffect)
            Desktop::Rule::windowEffects()->unregisterEffect(g_loopEffect);
    }
}
