// EyeContactTest - runs the plugin's Maxine engine on a still image outside Premiere.
//   EyeContactTest.exe in.bmp out.bmp [frames] [temporal 0/1]
// Input/output: uncompressed 24-bit BMP (e.g. ffmpeg -i clip.mp4 -frames:v 1 -pix_fmt bgr24 in.bmp).
// The same frame is fed `frames` times (default 10) to let temporal filtering settle.
// Details go to %TEMP%\EyeContact.log, a summary to stdout.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ColorUtil.h"
#include "MaxineEngine.h"

#pragma pack(push, 1)
struct BmpFileHeader { uint16_t type; uint32_t size; uint16_t r1, r2; uint32_t offset; };
struct BmpInfoHeader {
    uint32_t size; int32_t width, height; uint16_t planes, bpp; uint32_t compression, imageSize;
    int32_t xppm, yppm; uint32_t clrUsed, clrImportant;
};
#pragma pack(pop)

static bool ReadBmp(const char* path, std::vector<uint8_t>& bgr, int& w, int& h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return false;
    BmpFileHeader fh{}; BmpInfoHeader ih{};
    bool ok = fread(&fh, sizeof fh, 1, f) == 1 && fread(&ih, sizeof ih, 1, f) == 1 && fh.type == 0x4D42 &&
              ih.bpp == 24 && ih.compression == 0;
    if (!ok) { fclose(f); return false; }
    w = ih.width;
    h = ih.height < 0 ? -ih.height : ih.height;
    const bool bottomUp = ih.height > 0;
    const size_t stride = (static_cast<size_t>(w) * 3 + 3) & ~size_t(3);
    std::vector<uint8_t> row(stride);
    bgr.resize(static_cast<size_t>(w) * h * 3);
    fseek(f, fh.offset, SEEK_SET);
    for (int y = 0; y < h && ok; ++y) {
        ok = fread(row.data(), 1, stride, f) == stride;
        const int dy = bottomUp ? h - 1 - y : y;
        memcpy(&bgr[static_cast<size_t>(dy) * w * 3], row.data(), static_cast<size_t>(w) * 3);
    }
    fclose(f);
    return ok;
}

static bool WriteBmp(const char* path, const std::vector<uint8_t>& bgr, int w, int h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return false;
    const size_t stride = (static_cast<size_t>(w) * 3 + 3) & ~size_t(3);
    BmpFileHeader fh{0x4D42, static_cast<uint32_t>(54 + stride * h), 0, 0, 54};
    BmpInfoHeader ih{40, w, h, 1, 24, 0, static_cast<uint32_t>(stride * h), 2835, 2835, 0, 0};
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; --y) {
        memcpy(row.data(), &bgr[static_cast<size_t>(y) * w * 3], static_cast<size_t>(w) * 3);
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: EyeContactTest in.bmp out.bmp [frames=10] [temporal=1]\n");
        return 2;
    }
    // Diagnostics: EYECONTACT_TEST_PRELOAD="a.dll;b.dll" loads DLLs first, imitating a host such as
    // Premiere that already has its own copies of cudart / libcrypto in the process.
    if (const char* pre = getenv("EYECONTACT_TEST_PRELOAD")) {
        std::string list = pre;
        size_t pos = 0;
        while (pos < list.size()) {
            size_t end = list.find(';', pos);
            if (end == std::string::npos) end = list.size();
            std::string p = list.substr(pos, end - pos);
            if (!p.empty()) printf("preload %s -> %s\n", p.c_str(), LoadLibraryA(p.c_str()) ? "ok" : "FAILED");
            pos = end + 1;
        }
    }
    const int frames = argc > 3 ? atoi(argv[3]) : 10;
    EngineConfig cfg;
    cfg.temporal = argc > 4 ? atoi(argv[4]) != 0 : true;

    std::vector<uint8_t> src;
    int w = 0, h = 0;
    if (!ReadBmp(argv[1], src, w, h)) { printf("cannot read 24-bit BMP %s\n", argv[1]); return 2; }
    printf("input %dx%d, %d frames, temporal=%d\n", w, h, frames, (int)cfg.temporal);

    // Diagnostics: EYECONTACT_TEST_FOREIGN_CTX=1 makes a different "host" CUDA context current on
    // every frame, the way Premiere's GPU render threads do.
    using PFN_cuInit = int(__stdcall*)(unsigned);
    using PFN_cuCtxCreate = int(__stdcall*)(void**, unsigned, int);
    using PFN_cuCtxSetCurrent = int(__stdcall*)(void*);
    PFN_cuCtxSetCurrent setCurrent = nullptr;
    void* hostCtx[2] = {};
    if (const char* fc = getenv("EYECONTACT_TEST_FOREIGN_CTX"); fc && fc[0] == '1') {
        HMODULE cuda = LoadLibraryA("nvcuda.dll");
        auto init = reinterpret_cast<PFN_cuInit>(GetProcAddress(cuda, "cuInit"));
        auto create = reinterpret_cast<PFN_cuCtxCreate>(GetProcAddress(cuda, "cuCtxCreate_v2"));
        setCurrent = reinterpret_cast<PFN_cuCtxSetCurrent>(GetProcAddress(cuda, "cuCtxSetCurrent"));
        init(0);
        create(&hostCtx[0], 0, 0);
        create(&hostCtx[1], 0, 0);
        printf("simulating host CUDA contexts %p / %p\n", hostCtx[0], hostCtx[1]);
    }

    std::vector<uint8_t> out;
    int ok = 0, fail = 0;
    std::string err;

    // Diagnostics: EYECONTACT_TEST_WARMUP=other.bmp (same size) is processed once first - e.g. a frame
    // without a face, like Premiere's "Media pending" placeholder.
    if (const char* wu = getenv("EYECONTACT_TEST_WARMUP")) {
        std::vector<uint8_t> warm;
        int ww = 0, wh = 0;
        if (ReadBmp(wu, warm, ww, wh) && ww == w && wh == h) {
            bool r = MaxineEngine::Instance().Process(warm.data(), w, h, cfg, &err);
            printf("warm-up frame: %s\n", r ? "ok" : err.c_str());
        }
    }
    // Diagnostics: EYECONTACT_TEST_ALTERNATE=other.bmp replaces every 3rd frame (cutaways / no face).
    std::vector<uint8_t> alt;
    if (const char* al = getenv("EYECONTACT_TEST_ALTERNATE")) {
        int aw = 0, ah = 0;
        if (!ReadBmp(al, alt, aw, ah) || aw != w || ah != h) alt.clear();
    }
    int okFace = 0, faceFrames = 0;
    for (int i = 0; i < frames; ++i) {
        if (setCurrent) setCurrent(hostCtx[i % 2]);
        const bool useAlt = !alt.empty() && i % 3 == 1;
        out = useAlt ? alt : src;
        bool res = false;
        // Diagnostics: EYECONTACT_TEST_THREADS=1 processes every frame on a new thread, like Premiere.
        if (const char* th = getenv("EYECONTACT_TEST_THREADS"); th && th[0] == '1') {
            std::thread t([&] { res = MaxineEngine::Instance().Process(out.data(), w, h, cfg, &err); });
            t.join();
        } else {
            res = MaxineEngine::Instance().Process(out.data(), w, h, cfg, &err);
        }
        if (res) ++ok;
        else { ++fail; if (fail <= 3) printf("frame %d: %s\n", i, err.c_str()); }
        if (!useAlt) { ++faceFrames; if (res) ++okFace; }
    }
    if (!alt.empty()) printf("main-image frames ok: %d/%d\n", okFace, faceFrames);
    if (!alt.empty()) out = src, MaxineEngine::Instance().Process(out.data(), w, h, cfg, &err);  // end on main image
    if (const char* kc = getenv("EYECONTACT_TEST_KEEPCOLOR"); kc && kc[0] == '1') {
        for (size_t i = 0; i < src.size(); i += 3) {
            float b = out[i], g = out[i + 1], r = out[i + 2];
            KeepSourceColor(src[i], src[i + 1], src[i + 2], b, g, r);
            auto c8 = [](float v) { return static_cast<uint8_t>(v < 0.f ? 0.f : v > 255.f ? 255.f : v + 0.5f); };
            out[i] = c8(b); out[i + 1] = c8(g); out[i + 2] = c8(r);
        }
        printf("keep original color applied\n");
    }
    size_t changed = 0;
    for (size_t i = 0; i < src.size(); i += 3)
        if (src[i] != out[i] || src[i + 1] != out[i + 1] || src[i + 2] != out[i + 2]) ++changed;
    printf("ok=%d fail=%d, changed pixels in last frame: %zu\n", ok, fail, changed);
    if (!WriteBmp(argv[2], out, w, h)) { printf("cannot write %s\n", argv[2]); return 2; }
    MaxineEngine::Instance().Shutdown();
    return fail == 0 ? 0 : 1;
}
