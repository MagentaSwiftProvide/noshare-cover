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
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#define private public
#include "managers/screenshare/ScreenshareManager.hpp"
#undef private

#include "desktop/state/WindowState.hpp"
#include "desktop/view/Window.hpp"
#include "managers/fullscreen/FullscreenController.hpp"
#include "plugins/PluginAPI.hpp"
#include "render/pass/TexPassElement.hpp"

static HANDLE              PHANDLE = nullptr;
static CFunctionHook*       g_hook  = nullptr;
static SP<Render::ITexture> g_cover;

enum class eKind { None, Still, Gif, Video };
static eKind g_kind = eKind::None;
static bool g_missing = false;
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
    std::filesystem::file_time_type mtime{};
} g_conf;

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
} g_vid;

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
} g_gif;

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

static std::string trim(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return s;
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
    const int x1 = std::min(left + width, g_gif.w);
    const int y1 = std::min(top + height, g_gif.h);
    for (int y = y0; y < y1; ++y) {
        auto* row = g_gif.canvas.data() + y * g_gif.w;
        std::fill(row + x0, row + x1, color);
    }
}

static void blitGifFrame(int index) {
    const auto& frame = g_gif.frames[index];
    const auto& image = g_gif.file->SavedImages[index];
    const auto* map   = image.ImageDesc.ColorMap ? image.ImageDesc.ColorMap : g_gif.file->SColorMap;
    if (!map || !image.RasterBits || frame.width <= 0 || frame.height <= 0)
        return;

    for (int y = 0; y < frame.height; ++y) {
        const int dy = frame.top + y;
        if (dy < 0 || dy >= g_gif.h)
            continue;
        for (int x = 0; x < frame.width; ++x) {
            const int dx = frame.left + x;
            if (dx < 0 || dx >= g_gif.w)
                continue;
            const int px = image.RasterBits[y * frame.width + x];
            if (px == frame.transparent || px >= map->ColorCount)
                continue;
            const auto& c = map->Colors[px];
            g_gif.canvas[dy * g_gif.w + dx] = packPixel(c.Red, c.Green, c.Blue, 255);
        }
    }
}

static void applyDisposal(int index) {
    const auto& frame = g_gif.frames[index];
    if (frame.disposal == 2)
        clearRect(frame.left, frame.top, frame.width, frame.height, g_gif.bg);
    else if (frame.disposal == 3 && g_gif.backup.size() == g_gif.canvas.size())
        g_gif.canvas = g_gif.backup;
}

static void stepGif() {
    const int count = static_cast<int>(g_gif.frames.size());
    const int next  = g_gif.index + 1;
    if (next < 0 || next >= count)
        return;
    if (g_gif.index >= 0)
        applyDisposal(g_gif.index);
    if (g_gif.frames[next].disposal == 3)
        g_gif.backup = g_gif.canvas;
    blitGifFrame(next);
    g_gif.index = next;
}

static void uploadCover() {
    if (!g_pHyprRenderer || g_gif.canvas.empty())
        return;

    if (!g_cover || !g_cover->m_texID || g_cover->m_size.x != g_gif.w || g_cover->m_size.y != g_gif.h) {
        auto* surf = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(g_gif.canvas.data()), CAIRO_FORMAT_ARGB32, g_gif.w, g_gif.h, g_gif.w * 4);
        if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            if (surf)
                cairo_surface_destroy(surf);
            return;
        }
        g_cover = g_pHyprRenderer->createTexture(surf);
        cairo_surface_destroy(surf);
        return;
    }

    const CRegion damage{0.0, 0.0, static_cast<double>(g_gif.w), static_cast<double>(g_gif.h)};
    g_cover->update(DRM_FORMAT_ARGB8888, reinterpret_cast<uint8_t*>(g_gif.canvas.data()), static_cast<uint32_t>(g_gif.w * 4), damage);
}

static int frameAt(int64_t pos) {
    int64_t acc = 0;
    for (int i = 0; i < static_cast<int>(g_gif.frames.size()); ++i) {
        acc += g_gif.frames[i].delayMs;
        if (pos < acc)
            return i;
    }
    return static_cast<int>(g_gif.frames.size()) - 1;
}

static void showGifFrame(int target) {
    if (target == g_gif.index)
        return;
    if (target < g_gif.index) {
        std::fill(g_gif.canvas.begin(), g_gif.canvas.end(), g_gif.bg);
        g_gif.index = -1;
    }
    int guard = 0;
    while (g_gif.index != target && guard++ < static_cast<int>(g_gif.frames.size()) + 2)
        stepGif();
    uploadCover();
}

static void syncGif() {
    if (!g_gif.file || g_gif.frames.empty() || g_gif.totalMs < 1)
        return;
    const auto now = nowMs();
    if (g_gif.t0 == 0)
        g_gif.t0 = now;
    const double speed = g_conf.speed > 0.0 ? g_conf.speed : 1.0;
    auto         elapsed = static_cast<int64_t>((now - g_gif.t0) * speed);
    if (elapsed < 0)
        elapsed = 0;
    if (!g_conf.loop)
        elapsed = std::min(elapsed, g_gif.totalMs - 1);
    else
        elapsed %= g_gif.totalMs;
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

    g_gif.file = file;
    g_gif.w    = file->SWidth;
    g_gif.h    = file->SHeight;
    g_gif.canvas.assign(static_cast<size_t>(g_gif.w) * g_gif.h, 0);
    g_gif.frames.resize(file->ImageCount);

    if (file->SColorMap && file->SBackGroundColor >= 0 && file->SBackGroundColor < file->SColorMap->ColorCount) {
        const auto& c = file->SColorMap->Colors[file->SBackGroundColor];
        g_gif.bg      = packPixel(c.Red, c.Green, c.Blue, 255);
    }
    std::fill(g_gif.canvas.begin(), g_gif.canvas.end(), g_gif.bg);

    for (int i = 0; i < file->ImageCount; ++i) {
        const auto& image = file->SavedImages[i];
        int         transparent = -1;
        const int   packed      = extensionValue(image, &transparent);
        auto&       frame       = g_gif.frames[i];
        frame.delayMs           = packed & 0xffff;
        frame.disposal          = (packed >> 16) & 0x7;
        frame.transparent       = transparent;
        frame.left              = image.ImageDesc.Left;
        frame.top               = image.ImageDesc.Top;
        frame.width             = image.ImageDesc.Width;
        frame.height            = image.ImageDesc.Height;
        g_gif.totalMs += frame.delayMs;
    }

    showGifFrame(0);
    g_kind = eKind::Gif;
    return g_cover && g_cover->ok() && g_cover->m_texID;
}

static bool loadPng(const std::string& path) {
    auto* surf = cairo_image_surface_create_from_png(path.c_str());
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS || cairo_image_surface_get_width(surf) <= 0) {
        if (surf)
            cairo_surface_destroy(surf);
        return false;
    }

    g_cover = g_pHyprRenderer->createTexture(surf);
    cairo_surface_destroy(surf);
    g_kind  = eKind::Still;
    return g_cover && g_cover->ok() && g_cover->m_texID;
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

    g_gif.w = static_cast<int>(cinfo.output_width);
    g_gif.h = static_cast<int>(cinfo.output_height);
    if (g_gif.w < 1 || g_gif.h < 1) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(file);
        return false;
    }

    g_gif.canvas.assign(static_cast<size_t>(g_gif.w) * g_gif.h, 0);
    std::vector<uint8_t> row(static_cast<size_t>(g_gif.w) * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* scan = row.data();
        jpeg_read_scanlines(&cinfo, &scan, 1);
        const int y = static_cast<int>(cinfo.output_scanline) - 1;
        for (int x = 0; x < g_gif.w; ++x)
            g_gif.canvas[y * g_gif.w + x] = packPixel(row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 255);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(file);
    uploadCover();
    g_kind = eKind::Still;
    return g_cover && g_cover->ok() && g_cover->m_texID;
}

static void closeVideo() {
    if (g_vid.pkt)
        av_packet_free(&g_vid.pkt);
    if (g_vid.frame)
        av_frame_free(&g_vid.frame);
    if (g_vid.sws)
        sws_freeContext(g_vid.sws);
    if (g_vid.dec)
        avcodec_free_context(&g_vid.dec);
    if (g_vid.fmt)
        avformat_close_input(&g_vid.fmt);
    g_vid = {};
}

static bool presentVideoFrame() {
    if (!g_vid.frame || g_vid.frame->width < 1 || g_vid.frame->height < 1)
        return false;

    g_vid.sws = sws_getCachedContext(g_vid.sws, g_vid.frame->width, g_vid.frame->height, static_cast<AVPixelFormat>(g_vid.frame->format), g_gif.w, g_gif.h, AV_PIX_FMT_BGRA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!g_vid.sws)
        return false;

    uint8_t* dstData[4]  = {reinterpret_cast<uint8_t*>(g_gif.canvas.data()), nullptr, nullptr, nullptr};
    int      dstStride[4] = {g_gif.w * 4, 0, 0, 0};
    sws_scale(g_vid.sws, g_vid.frame->data, g_vid.frame->linesize, 0, g_vid.frame->height, dstData, dstStride);
    uploadCover();
    return g_cover && g_cover->ok() && g_cover->m_texID;
}

static bool pullVideoFrame() {
    auto* st = g_vid.fmt->streams[g_vid.stream];
    for (int spins = 0; spins < 256; ++spins) {
        const int got = avcodec_receive_frame(g_vid.dec, g_vid.frame);
        if (got == 0) {
            const int64_t pts = g_vid.frame->best_effort_timestamp != AV_NOPTS_VALUE ? g_vid.frame->best_effort_timestamp : g_vid.frame->pts;
            g_vid.lastUs      = pts == AV_NOPTS_VALUE ? g_vid.lastUs + 1 : av_rescale_q(pts, st->time_base, AV_TIME_BASE_Q);
            return true;
        }
        if (got != AVERROR(EAGAIN) && got != AVERROR_EOF)
            return false;

        if (g_vid.draining)
            return false;

        if (av_read_frame(g_vid.fmt, g_vid.pkt) < 0) {
            avcodec_send_packet(g_vid.dec, nullptr);
            g_vid.draining = true;
            continue;
        }
        if (g_vid.pkt->stream_index == g_vid.stream)
            avcodec_send_packet(g_vid.dec, g_vid.pkt);
        av_packet_unref(g_vid.pkt);
    }
    return false;
}

static void seekVideo(int64_t elapsedUs) {
    auto* st = g_vid.fmt->streams[g_vid.stream];
    const auto pts = av_rescale_q(elapsedUs, AV_TIME_BASE_Q, st->time_base);
    av_seek_frame(g_vid.fmt, g_vid.stream, std::max<int64_t>(pts, 0), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(g_vid.dec);
    g_vid.lastUs   = -1;
    g_vid.draining = false;
}

static bool loadVideo(const std::string& path) {
    closeVideo();
    if (avformat_open_input(&g_vid.fmt, path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(g_vid.fmt, nullptr) < 0)
        return false;

    const AVCodec* codec = nullptr;
    g_vid.stream         = av_find_best_stream(g_vid.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (g_vid.stream < 0 || !codec)
        return false;

    g_vid.dec = avcodec_alloc_context3(codec);
    if (!g_vid.dec || avcodec_parameters_to_context(g_vid.dec, g_vid.fmt->streams[g_vid.stream]->codecpar) < 0 || avcodec_open2(g_vid.dec, codec, nullptr) < 0)
        return false;

    g_vid.frame = av_frame_alloc();
    g_vid.pkt   = av_packet_alloc();
    g_gif.w     = g_vid.dec->width;
    g_gif.h     = g_vid.dec->height;
    if (!g_vid.frame || !g_vid.pkt || g_gif.w < 1 || g_gif.h < 1)
        return false;

    auto* st = g_vid.fmt->streams[g_vid.stream];
    if (st->duration > 0)
        g_vid.durationUs = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
    else if (g_vid.fmt->duration > 0)
        g_vid.durationUs = g_vid.fmt->duration;

    g_gif.canvas.assign(static_cast<size_t>(g_gif.w) * g_gif.h, packPixel(0, 0, 0, 255));
    if (!pullVideoFrame() || !presentVideoFrame())
        return false;

    g_kind = eKind::Video;
    return true;
}

static void syncVideo() {
    if (g_kind != eKind::Video || !g_vid.fmt || g_vid.stream < 0)
        return;

    const auto now = nowMs();
    if (g_vid.t0 == 0)
        g_vid.t0 = now;

    const double speed = g_conf.speed > 0.0 ? g_conf.speed : 1.0;
    int64_t      elapsedUs = static_cast<int64_t>((now - g_vid.t0) * speed * 1000.0);
    if (elapsedUs < 0)
        elapsedUs = 0;
    if (g_vid.durationUs > 0) {
        if (g_conf.loop)
            elapsedUs %= g_vid.durationUs;
        else
            elapsedUs = std::min(elapsedUs, g_vid.durationUs);
    }

    if (g_vid.lastUs < 0 || elapsedUs + 50000 < g_vid.lastUs)
        seekVideo(elapsedUs);

    bool presented = false;
    int  guard     = 0;
    while (g_vid.lastUs < elapsedUs && guard++ < 48) {
        if (!pullVideoFrame()) {
            if (g_conf.loop && g_vid.durationUs > 0) {
                seekVideo(0);
                g_vid.t0 = now;
            }
            break;
        }
        presented = true;
    }
    if (presented)
        presentVideoFrame();
}

static void closeGif() {
    if (!g_gif.file)
        return;
    int err = 0;
    DGifCloseFile(g_gif.file, &err);
    g_gif.file = nullptr;
    g_gif.frames.clear();
    g_gif.index   = -1;
    g_gif.totalMs = 0;
    g_gif.t0      = 0;
}

static void unloadMedia() {
    closeGif();
    closeVideo();
    g_gif.canvas.clear();
    g_kind = eKind::None;
}

static std::string defaultMediaPath() {
    for (const char* name : {"noshare-cover.gif", "noshare-cover.jpg", "noshare-cover.jpeg", "noshare-cover.png", "noshare-cover.mp4"}) {
        if (std::filesystem::exists(configFile(name)))
            return "~/.config/hypr/" + std::string{name};
    }
    return "~/.config/hypr/noshare-cover.gif";
}

static void writeDefaultConfig() {
    const auto path = configFile("noshare-cover.conf");
    if (std::filesystem::exists(path))
        return;
    std::ofstream out(path);
    out << "# noshare-cover. Меняешь file и перезагружаешь плагин, либо просто сохраняешь файл: подхватится само.\n"
        << "file = " << defaultMediaPath() << "\n"
        << "loop = true\n"
        << "speed = 1.0\n";
}

static bool readConfig() {
    writeDefaultConfig();
    const auto path = configFile("noshare-cover.conf");
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (g_conf.ready && !ec && mtime == g_conf.mtime)
        return false;

    SConf next;
    next.mtime = mtime;
    next.ready = true;
    std::ifstream in(path);
    std::string   line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const auto key = lower(trim(line.substr(0, eq)));
        const auto val = trim(line.substr(eq + 1));
        if (key == "file")
            next.file = expandHome(val);
        else if (key == "loop")
            next.loop = !(val == "0" || lower(val) == "false" || lower(val) == "no" || lower(val) == "off");
        else if (key == "speed") {
            try {
                next.speed = std::stod(val);
            } catch (...) {
                next.speed = 1.0;
            }
        }
    }
    if (next.file.empty())
        next.file = expandHome(defaultMediaPath());

    const bool changed = !g_conf.ready || next.file != g_conf.file || next.loop != g_conf.loop || next.speed != g_conf.speed;
    g_conf             = next;
    return changed;
}

static bool loadMedia() {
    const auto path = g_conf.file;
    if (path.empty() || !std::filesystem::exists(path)) {
        g_missing = true;
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
        notifyOnce("noshare-cover: не знаю формат " + ext);
        return false;
    }

    if (!ok) {
        unloadMedia();
        notifyOnce("noshare-cover: не открылся " + path);
        return false;
    }
    g_error.clear();
    g_missing = false;
    return true;
}

static bool ensureCover() {
    if (readConfig()) {
        unloadMedia();
        g_missing = false;
        g_error.clear();
    }
    if (g_kind != eKind::None)
        return g_cover && g_cover->ok() && g_cover->m_texID;
    if (g_missing) {
        std::error_code ec;
        if (g_conf.file.empty() || !std::filesystem::exists(g_conf.file, ec))
            return false;
        g_missing = false;
        g_error.clear();
    } else if (!g_error.empty())
        return false;
    if (!loadMedia())
        return false;
    return g_cover && g_cover->ok() && g_cover->m_texID;
}

using RenderMonitorFn = void (*)(Screenshare::CScreenshareFrame*);

static void paintCovers(Screenshare::CScreenshareFrame* self) {
    if (!self || !self->m_session || !g_pHyprRenderer || !ensureCover())
        return;

    if (g_kind == eKind::Gif)
        syncGif();
    else if (g_kind == eKind::Video)
        syncVideo();

    const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!mon)
        return;

    const auto capturePos = self->m_session->m_captureBox.pos();

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

        const bool dontRound = capturePos != Vector2D{} || (Fullscreen::controller() && Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN));
        const int  rounding  = dontRound ? 0 : static_cast<int>(std::lround(w->rounding() * mon->m_scale));
        const auto roundPow  = dontRound ? 2.F : w->roundingPower();

        g_pHyprRenderer->draw(CTexPassElement::SRenderData{
                                  .tex           = g_cover,
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

    if (!addr) {
        HyprlandAPI::addNotification(handle, "noshare-cover: не нашёл renderMonitor", CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 5000);
        return {"noshare-cover", "missing symbol", "vlad", "0.1"};
    }

    if (!ensureCover())
        return {"noshare-cover", "cover image missing", "vlad", "0.1"};

    g_hook = HyprlandAPI::createFunctionHook(handle, addr, reinterpret_cast<void*>(&hkRenderMonitor));
    if (!g_hook || !g_hook->hook()) {
        HyprlandAPI::addNotification(handle, "noshare-cover: хук не встал", CHyprColor{1.F, 0.2F, 0.2F, 1.F}, 5000);
        return {"noshare-cover", "hook failed", "vlad", "0.1"};
    }

    return {"noshare-cover", "image instead of no_screen_share black box", "vlad", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    unloadMedia();
    g_cover.reset();
}
