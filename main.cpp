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
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <pwd.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define private public
#include "managers/screenshare/ScreenshareManager.hpp"
#undef private

#include "desktop/rule/Engine.hpp"
#include "desktop/rule/windowRule/WindowRule.hpp"
#include "desktop/state/WindowState.hpp"
#if __has_include("desktop/view/window/Window.hpp")
#include "desktop/view/window/Window.hpp"
#include "desktop/view/window/WindowPresentation.hpp"
#define NOSHARE_SPLIT_WINDOW 1
#else
#include "desktop/view/Window.hpp"
#endif
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
    AVFrame*         sw     = nullptr;
    AVPacket*        pkt    = nullptr;
    int              stream = -1;
    int64_t          durationUs = 0;
    int64_t          lastUs     = -1;
    int64_t          t0         = 0;
    bool             draining   = false;
    bool             gpu        = false;
    pid_t            gpuPid     = -1;
    int              gpuFd      = -1;
    int              frameMs    = 33;
    int              gpuW       = 0;
    int              gpuH       = 0;
    bool             quit       = false;
    bool             failed     = false;
    std::string      path;
    int64_t          targetUs   = -1;
    int64_t          seen       = -1;
    int64_t          watchMs    = 0;
    uint64_t         gen        = 0;
    uint64_t         uploaded   = 0;
    int              pendingW   = 0;
    int              pendingH   = 0;
    std::vector<uint32_t> pending;
    std::mutex              mu;
    std::condition_variable cv;
    std::thread             worker;
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

extern char** environ;

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
    if (!active)
        return;
    auto& v = active->vid;
    {
        std::lock_guard lk(v.mu);
        v.quit = true;
    }
    if (v.gpuPid > 0)
        kill(v.gpuPid, SIGKILL);
    if (v.gpuFd >= 0) {
        close(v.gpuFd);
        v.gpuFd = -1;
    }
    v.cv.notify_all();
    if (v.worker.joinable())
        v.worker.join();
    if (v.gpuPid > 0) {
        waitpid(v.gpuPid, nullptr, 0);
        v.gpuPid = -1;
    }

    if (v.pkt)
        av_packet_free(&v.pkt);
    if (v.frame)
        av_frame_free(&v.frame);
    if (v.sw)
        av_frame_free(&v.sw);
    if (v.sws)
        sws_freeContext(v.sws);
    v.sws = nullptr;
    if (v.dec)
        avcodec_free_context(&v.dec);
    if (v.fmt)
        avformat_close_input(&v.fmt);

    v.stream     = -1;
    v.durationUs = 0;
    v.lastUs     = -1;
    v.t0         = 0;
    v.draining   = false;
    v.gpu        = false;
    v.gpuPid     = -1;
    v.gpuFd      = -1;
    v.frameMs    = 33;
    v.gpuW       = 0;
    v.gpuH       = 0;
    v.quit       = false;
    v.failed     = false;
    v.targetUs   = -1;
    v.seen       = -1;
    v.watchMs    = 0;
    v.gen        = 0;
    v.uploaded   = 0;
    v.pendingW   = 0;
    v.pendingH   = 0;
    v.pending.clear();
    v.path.clear();
}

static bool pullVideoFrame(SCover* self) {
    auto& v  = self->vid;
    auto* st = v.fmt->streams[v.stream];
    for (int spins = 0; spins < 256; ++spins) {
        const int got = avcodec_receive_frame(v.dec, v.frame);
        if (got == 0) {
            const int64_t pts = v.frame->best_effort_timestamp != AV_NOPTS_VALUE ? v.frame->best_effort_timestamp : v.frame->pts;
            v.lastUs          = pts == AV_NOPTS_VALUE ? v.lastUs + 1 : av_rescale_q(pts, st->time_base, AV_TIME_BASE_Q);
            return true;
        }
        if (got != AVERROR(EAGAIN) && got != AVERROR_EOF)
            return false;
        if (v.draining)
            return false;
        if (av_read_frame(v.fmt, v.pkt) < 0) {
            avcodec_send_packet(v.dec, nullptr);
            v.draining = true;
            continue;
        }
        if (v.pkt->stream_index == v.stream)
            avcodec_send_packet(v.dec, v.pkt);
        av_packet_unref(v.pkt);
    }
    return false;
}

static void seekVideo(SCover* self, int64_t elapsedUs) {
    auto& v    = self->vid;
    auto* st   = v.fmt->streams[v.stream];
    const auto pts = av_rescale_q(elapsedUs, AV_TIME_BASE_Q, st->time_base);
    av_seek_frame(v.fmt, v.stream, std::max<int64_t>(pts, 0), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(v.dec);
    v.lastUs   = -1;
    v.draining = false;
}

static bool publishFrame(SCover* self) {
    auto&     v   = self->vid;
    AVFrame*  cpu = v.frame;
    const int w   = cpu->width;
    const int h = cpu->height;
    if (w < 1 || h < 1)
        return false;

    std::vector<uint32_t> canvas(static_cast<size_t>(w) * static_cast<size_t>(h));
    uint8_t*              dst[4]    = {reinterpret_cast<uint8_t*>(canvas.data()), nullptr, nullptr, nullptr};
    int                   stride[4] = {w * 4, 0, 0, 0};
    v.sws = sws_getCachedContext(v.sws, w, h, static_cast<AVPixelFormat>(cpu->format), w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!v.sws)
        return false;
    sws_scale(v.sws, cpu->data, cpu->linesize, 0, h, dst, stride);

    {
        std::lock_guard lk(v.mu);
        v.pending.swap(canvas);
        v.pendingW = w;
        v.pendingH = h;
        v.gen++;
    }
    v.cv.notify_all();
    return true;
}

static bool openDecoder(SCover* self, const AVCodecParameters* par, AVRational timeBase);

static bool openInput(SCover* self) {
    auto& v = self->vid;
    if (avformat_open_input(&v.fmt, v.path.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(v.fmt, nullptr) < 0)
        return false;

    const AVCodec* ignored = nullptr;
    v.stream               = av_find_best_stream(v.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &ignored, 0);
    if (v.stream < 0)
        return false;

    auto* st = v.fmt->streams[v.stream];
    if (!openDecoder(self, st->codecpar, st->time_base))
        return false;

    v.frame = av_frame_alloc();
    v.sw    = av_frame_alloc();
    v.pkt   = av_packet_alloc();
    if (!v.frame || !v.sw || !v.pkt)
        return false;

    int64_t duration = 0;
    if (st->duration > 0)
        duration = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
    else if (v.fmt->duration > 0)
        duration = v.fmt->duration;
    {
        std::lock_guard lk(v.mu);
        v.durationUs = duration;
    }
    return true;
}

static const char* cuvidFor(const std::string& codec) {
    if (codec == "av1")
        return "av1_cuvid";
    if (codec == "h264")
        return "h264_cuvid";
    if (codec == "hevc")
        return "hevc_cuvid";
    if (codec == "vp9")
        return "vp9_cuvid";
    if (codec == "vp8")
        return "vp8_cuvid";
    if (codec == "mpeg2video")
        return "mpeg2_cuvid";
    if (codec == "mpeg4")
        return "mpeg4_cuvid";
    if (codec == "mjpeg")
        return "mjpeg_cuvid";
    return nullptr;
}

static bool spawnCapture(const std::vector<std::string>& args, pid_t& pid, int& readFd) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0)
        return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        pid = -1;
        return false;
    }
    readFd = pipefd[0];
    return true;
}

static void stopGpu(SVideo& v) {
    if (v.gpuPid > 0)
        kill(v.gpuPid, SIGKILL);
    if (v.gpuFd >= 0) {
        close(v.gpuFd);
        v.gpuFd = -1;
    }
    if (v.gpuPid > 0) {
        waitpid(v.gpuPid, nullptr, 0);
        v.gpuPid = -1;
    }
}

static bool readFull(int fd, uint8_t* dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        const ssize_t r = read(fd, dst + got, n - got);
        if (r <= 0)
            return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

static bool probeVideo(const std::string& path, std::string& codec, int& w, int& h, int& frameMs) {
    pid_t pid = -1;
    int   fd  = -1;
    if (!spawnCapture({"/usr/bin/ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name,width,height,avg_frame_rate", "-of", "csv=p=0", path}, pid, fd))
        return false;
    std::string out;
    char        buf[512];
    ssize_t     n = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        out.append(buf, static_cast<size_t>(n));
    close(fd);
    waitpid(pid, nullptr, 0);
    std::string rate;
    std::stringstream ss(out);
    if (!std::getline(ss, codec, ','))
        return false;
    std::string sw, sh;
    if (!std::getline(ss, sw, ',') || !std::getline(ss, sh, ',') || !std::getline(ss, rate))
        return false;
    w = std::atoi(sw.c_str());
    h = std::atoi(sh.c_str());
    int num = 0, den = 1;
    if (std::sscanf(rate.c_str(), "%d/%d", &num, &den) == 2 && num > 0 && den > 0)
        frameMs = std::clamp(den * 1000 / num, 1, 1000);
    else
        frameMs = 33;
    return w > 0 && h > 0 && cuvidFor(codec);
}

static bool startGpu(SCover* self) {
    auto&       v = self->vid;
    std::string codec;
    int         w = 0, h = 0, frameMs = 33;
    if (!probeVideo(v.path, codec, w, h, frameMs))
        return false;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    if (bytes == 0 || bytes > 64u * 1024u * 1024u)
        return false;

    std::vector<std::string> args = {"/usr/bin/ffmpeg", "-nostdin", "-loglevel", "error", "-hwaccel", "cuda", "-c:v", cuvidFor(codec)};
    if (self->loop)
        args.insert(args.end(), {"-stream_loop", "-1"});
    args.insert(args.end(), {"-i", v.path, "-an", "-vf", "format=bgra", "-f", "rawvideo", "-pix_fmt", "bgra", "pipe:1"});

    pid_t pid = -1;
    int   fd  = -1;
    if (!spawnCapture(args, pid, fd))
        return false;

    pollfd pfd{fd, POLLIN, 0};
    const int pr = poll(&pfd, 1, 2000);
    if (pr <= 0 || (pfd.revents & (POLLERR | POLLNVAL)) || ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN))) {
        v.gpuPid = pid;
        v.gpuFd  = fd;
        stopGpu(v);
        return false;
    }

    v.gpu      = true;
    v.gpuPid   = pid;
    v.gpuFd    = fd;
    v.frameMs  = frameMs;
    v.gpuW     = w;
    v.gpuH     = h;
    return true;
}

static void publishRaw(SCover* self, std::vector<uint32_t>&& canvas, int w, int h) {
    auto& v = self->vid;
    {
        std::lock_guard lk(v.mu);
        v.pending.swap(canvas);
        v.pendingW = w;
        v.pendingH = h;
        v.gen++;
    }
    v.cv.notify_all();
}

static void playGpu(SCover* self) {
    auto&        v     = self->vid;
    const size_t bytes = static_cast<size_t>(v.gpuW) * static_cast<size_t>(v.gpuH) * 4;
    while (true) {
        {
            std::unique_lock lk(v.mu);
            v.cv.wait(lk, [&] { return v.quit || nowMs() - v.watchMs < 400; });
            if (v.quit)
                return;
        }

        std::vector<uint32_t> canvas(bytes / 4);
        if (!readFull(v.gpuFd, reinterpret_cast<uint8_t*>(canvas.data()), bytes))
            return;

        publishRaw(self, std::move(canvas), v.gpuW, v.gpuH);

        const double  speed  = self->speed > 0.0 ? self->speed : 1.0;
        const int64_t waitMs = std::clamp(static_cast<int64_t>(v.frameMs / speed), int64_t{1}, int64_t{1000});
        std::unique_lock lk(v.mu);
        v.cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] { return v.quit; });
        if (v.quit)
            return;
    }
}

static void videoWorker(SCover* self) {
    {
        std::lock_guard lk(self->vid.mu);
        if (self->vid.quit)
            return;
    }
    if (startGpu(self)) {
        playGpu(self);
        return;
    }
    if (!openInput(self)) {
        std::lock_guard lk(self->vid.mu);
        self->vid.failed = true;
        self->vid.cv.notify_all();
        return;
    }

    auto&   v        = self->vid;
    int64_t epoch    = 0;
    int64_t originUs = 0;
    bool    clockOn  = false;

    while (true) {
        bool wokeFromIdle = false;
        {
            std::unique_lock lk(v.mu);
            wokeFromIdle = nowMs() - v.watchMs >= 400;
            v.cv.wait(lk, [&] { return v.quit || nowMs() - v.watchMs < 400; });
            if (v.quit)
                return;
        }
        if (!clockOn || wokeFromIdle) {
            epoch    = nowMs();
            originUs = std::max<int64_t>(v.lastUs, 0);
            clockOn  = true;
        }

        if (!pullVideoFrame(self)) {
            if (self->loop && v.durationUs > 0 && v.draining) {
                seekVideo(self, 0);
                epoch    = nowMs();
                originUs = 0;
                continue;
            }
            std::unique_lock lk(v.mu);
            if (v.gen == 0)
                v.failed = true;
            v.cv.wait_for(lk, std::chrono::milliseconds(200), [&] { return v.quit; });
            if (v.quit || v.failed)
                return;
            continue;
        }

        const double  speed = self->speed > 0.0 ? self->speed : 1.0;
        const int64_t due   = epoch + static_cast<int64_t>((v.lastUs - originUs) / 1000.0 / speed);
        const int64_t now   = nowMs();
        if (due > now) {
            const auto waitMs = std::min<int64_t>(due - now, 1000);
            std::unique_lock lk(v.mu);
            v.cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] { return v.quit; });
            if (v.quit)
                return;
        } else if (now - due > 80) {
            continue;
        }

        publishFrame(self);
    }
}

static bool uploadPending() {
    std::vector<uint32_t> frame;
    int                   w = 0;
    int                   h = 0;
    {
        std::lock_guard lk(active->vid.mu);
        if (active->vid.gen == active->vid.uploaded || active->vid.pending.empty())
            return active->tex && active->tex->ok() && active->tex->m_texID;
        frame.swap(active->vid.pending);
        w = active->vid.pendingW;
        h = active->vid.pendingH;
        active->vid.uploaded = active->vid.gen;
    }
    active->gif.w = w;
    active->gif.h = h;
    active->gif.canvas.swap(frame);
    uploadCover();
    return active->tex && active->tex->ok() && active->tex->m_texID;
}

static bool openDecoder(SCover* self, const AVCodecParameters* par, AVRational timeBase) {
    auto&          v     = self->vid;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
        return false;
    v.dec = avcodec_alloc_context3(codec);
    if (!v.dec || avcodec_parameters_to_context(v.dec, par) < 0)
        return false;
    v.dec->pkt_timebase = timeBase;
    if (avcodec_open2(v.dec, codec, nullptr) < 0)
        return false;
    v.gpu = false;
    return true;
}

static bool loadVideo(const std::string& path) {
    closeVideo();
    active->vid.path = path;
    active->kind     = eKind::Video;
    {
        std::lock_guard lk(active->vid.mu);
        active->vid.targetUs = 0;
    }
    active->vid.worker = std::thread(videoWorker, active);
    return true;
}

static void syncVideo() {
    if (!active || active->kind != eKind::Video)
        return;

    int64_t duration = 0;
    {
        std::lock_guard lk(active->vid.mu);
        if (active->vid.failed)
            return;
        duration = active->vid.durationUs;
    }

    const auto now = nowMs();
    if (active->vid.t0 == 0)
        active->vid.t0 = now;

    const double speed     = active->speed > 0.0 ? active->speed : 1.0;
    int64_t      elapsedUs = static_cast<int64_t>((now - active->vid.t0) * speed * 1000.0);
    if (elapsedUs < 0)
        elapsedUs = 0;
    if (duration > 0) {
        if (active->loop)
            elapsedUs %= duration;
        else
            elapsedUs = std::min(elapsedUs, duration);
    }

    {
        std::lock_guard lk(active->vid.mu);
        active->vid.targetUs = elapsedUs;
        active->vid.watchMs  = now;
    }
    active->vid.cv.notify_all();
    uploadPending();
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
    if (cover->kind == eKind::Video) {
        bool failed = false;
        {
            std::lock_guard lk(cover->vid.mu);
            failed = cover->vid.failed;
        }
        if (failed) {
            if (cover->error.empty()) {
                cover->error = play.path;
                notifyOnce("noshare-cover: не открылся " + play.path);
            }
            return nullptr;
        }
        return cover;
    }
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

static float coverFade(PHLWINDOW w) {
#ifdef NOSHARE_SPLIT_WINDOW
    return w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
#else
    return w->alphaValue(Desktop::View::WINDOW_ALPHA_FADE) * w->alphaValue(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
#endif
}

static bool coverPinned(PHLWINDOW w) {
#ifdef NOSHARE_SPLIT_WINDOW
    return sc<bool>(w->m_state & Desktop::View::WINDOW_STATE_PINNED);
#else
    return w->m_pinned;
#endif
}

static float coverRounding(PHLWINDOW w) {
#ifdef NOSHARE_SPLIT_WINDOW
    return w->presentation().rounding();
#else
    return w->rounding();
#endif
}

static float coverRoundingPower(PHLWINDOW w) {
#ifdef NOSHARE_SPLIT_WINDOW
    return w->presentation().roundingPower();
#else
    return w->roundingPower();
#endif
}

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
        if (!ws && coverFade(w) != 0.F)
            continue;

        const auto renderOffset = ws && !coverPinned(w) && ws->m_renderOffset ? ws->m_renderOffset->value() : Vector2D{};
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
        if (!cover)
            continue;
        if (synced.insert(play.key()).second) {
            active = cover;
            if (cover->kind == eKind::Gif)
                syncGif();
            else if (cover->kind == eKind::Video)
                syncVideo();
        }
        if (!cover->tex || !cover->tex->ok() || !cover->tex->m_texID)
            continue;

        const bool dontRound = capturePos != Vector2D{} || (Fullscreen::controller() && Fullscreen::controller()->isFullscreen(w, Fullscreen::FSMODE_FULLSCREEN));
        const int  rounding  = dontRound ? 0 : static_cast<int>(std::lround(coverRounding(w) * mon->m_scale));
        const auto roundPow  = dontRound ? 2.F : coverRoundingPower(w);

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

    g_coverEffect = Desktop::Rule::windowEffects()->registerEffect("no_screen_share_cover:path_cover");
    g_speedEffect = Desktop::Rule::windowEffects()->registerEffect("no_screen_share_cover:speed");
    g_loopEffect  = Desktop::Rule::windowEffects()->registerEffect("no_screen_share_cover:loop");

    g_cfgFile  = Config::Values::makeConfigValue<Config::Values::CStringValue>("plugin:no_screen_share_cover:path_cover", "Default media for no_screen_share windows", Config::STRING{});
    g_cfgLoop  = Config::Values::makeConfigValue<Config::Values::CBoolValue>("plugin:no_screen_share_cover:loop", "Loop gif and video", true);
    g_cfgSpeed = Config::Values::makeConfigValue<Config::Values::CFloatValue>("plugin:no_screen_share_cover:speed", "Playback speed for gif and video", 1.F);
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
