// EyeContact - NVIDIA Maxine AR SDK GazeRedirection wrapper.
#include "MaxineEngine.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#include "nvAR.h"
#include "nvAR_defs.h"
#include "nvCVImage.h"
#include "nvCVStatus.h"

// Required by nvARProxy.cpp: folder the proxy loads nvARPose.dll from. Set in PreloadRuntime().
char* g_nvARSDKPath = nullptr;

namespace {

// ---------------------------------------------------------------- logging
// Everything goes to %TEMP%\EyeContact.log - the first place to look when the effect does nothing.
void Log(const char* fmt, ...) {
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 16) return;
    strcat_s(path, "EyeContact.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void LogV(const char* fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    Log("%s", buf);
}

// Debug mode: create an empty file %TEMP%\EyeContact.debug to dump frames as BMP next to the log.
bool DebugDumps() {
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 20) return false;
    strcat_s(path, "EyeContact.debug");
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

void DumpBgr(const char* tag, const uint8_t* bgr, int w, int h) {
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 64) return;
    char name[64];
    snprintf(name, sizeof(name), "EyeContact_%s_%dx%d.bmp", tag, w, h);
    strcat_s(path, name);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return;
    const uint32_t stride = (static_cast<uint32_t>(w) * 3 + 3) & ~3u;
    const uint32_t imgSize = stride * static_cast<uint32_t>(h);
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { memcpy(hdr + off, &v, 4); };
    put32(2, 54 + imgSize); put32(10, 54); put32(14, 40); put32(18, (uint32_t)w); put32(22, (uint32_t)h);
    hdr[26] = 1; hdr[28] = 24; put32(34, imgSize);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(stride, 0);
    for (int y = h - 1; y >= 0; --y) {  // BMP is bottom-up
        memcpy(row.data(), bgr + static_cast<size_t>(y) * w * 3, static_cast<size_t>(w) * 3);
        fwrite(row.data(), 1, stride, f);
    }
    fclose(f);
    Log("dumped %s", path);
}

std::string FrameStats(const uint8_t* bgr, int w, int h) {
    double sb = 0, sg = 0, sr = 0;
    const size_t n = static_cast<size_t>(w) * h;
    const size_t step = std::max<size_t>(1, n / 4096);  // sample ~4k pixels
    size_t cnt = 0;
    for (size_t i = 0; i < n; i += step, ++cnt) { sb += bgr[i * 3]; sg += bgr[i * 3 + 1]; sr += bgr[i * 3 + 2]; }
    char buf[96];
    snprintf(buf, sizeof(buf), "mean BGR=(%.0f,%.0f,%.0f)", sb / cnt, sg / cnt, sr / cnt);
    return buf;
}

std::string GetEnv(const char* name) {
    char* v = nullptr;
    size_t len = 0;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        std::string s(v);
        free(v);
        return s;
    }
    return {};
}

std::string StatusStr(NvCV_Status s) {
    const char* msg = NvCV_GetErrorStringFromCode(s);
    char buf[320];
    snprintf(buf, sizeof(buf), "%d (%s)", static_cast<int>(s), msg ? msg : "unknown");
    return buf;
}

bool FileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------- runtime preload
// Premiere ships its own CUDA / AI DLLs. To make sure Maxine's dependencies (TensorRT, cuDNN, ...)
// are resolved from the Maxine runtime folder and not from wherever Premiere happens to look,
// we load the two entry DLLs ourselves by full path with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR.
// The SDK proxy (nvARProxy.cpp / nvCVImageProxy.cpp) then gets these already-loaded modules.
bool PreloadRuntime(std::string* err) {
    static bool done = false, ok = false;
    static std::string lastErr;
    if (done) {
        if (!ok && err) *err = lastErr;
        return ok;
    }
    done = true;

    // Note: NV_AR_SDK_PATH is NOT a path for the SDK proxy (only the value "USE_APP_PATH" matters),
    // so we use our own variable for a custom runtime location.
    std::vector<std::string> roots;
    std::string env = GetEnv("MAXINE_AR_RUNTIME_DIR");
    if (!env.empty()) roots.push_back(env);
    std::string pf = GetEnv("ProgramFiles");
    roots.push_back((pf.empty() ? std::string("C:\\Program Files") : pf) + "\\NVIDIA Corporation\\NVIDIA AR SDK");

    for (const std::string& root : roots) {
        for (const std::string& dir : {root, root + "\\bin"}) {
            if (!FileExists(dir + "\\nvARPose.dll")) continue;
            // Tell the proxy to use exactly this folder.
            static std::string sdkPath;
            sdkPath = dir + "\\";
            g_nvARSDKPath = sdkPath.data();
            for (const char* dll : {"NVCVImage.dll", "nvARPose.dll"}) {
                std::string p = dir + "\\" + dll;
                HMODULE m = LoadLibraryExA(p.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (!m) {
                    lastErr = "Cannot load " + p + " (Win32 error " + std::to_string(GetLastError()) +
                              "). The Maxine AR SDK runtime may be incomplete or conflict with Premiere's DLLs.";
                    Log("%s", lastErr.c_str());
                    if (err) *err = lastErr;
                    return false;
                }
                Log("Loaded %s", p.c_str());
            }
            // nvARPose loads its dependencies (cuBLAS, NPP, cuFFT, nvrtc, TensorRT...) lazily by name, some
            // only when a face is first redirected. By then the host's DLL search path no longer includes
            // the runtime folder -> NvAR_Run fails with -1. So load them all now, by full path. DLLs the
            // host already has (Premiere ships cudart64_12 and libcrypto) are skipped: same-named modules
            // are shared, and Premiere's newer copies work with Maxine.
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((dir + "\\*.dll").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                int loaded = 0;
                do {
                    if (GetModuleHandleA(fd.cFileName)) continue;
                    std::string p = dir + "\\" + fd.cFileName;
                    if (LoadLibraryExA(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
                        ++loaded;
                    else
                        Log("warning: cannot preload %s (Win32 error %lu)", p.c_str(), GetLastError());
                } while (FindNextFileA(h, &fd));
                FindClose(h);
                Log("Preloaded %d runtime DLLs", loaded);
            }
            ok = true;
            return true;
        }
    }
    lastErr = "NVIDIA Maxine AR SDK runtime not found (nvARPose.dll). Install it or set MAXINE_AR_RUNTIME_DIR.";
    Log("%s", lastErr.c_str());
    if (err) *err = lastErr;
    return false;
}

// ---------------------------------------------------------------- CUDA context guard
// Premiere renders on threads where its own CUDA context is current. Maxine uses the CUDA runtime,
// which works in whatever context is current - so the model would be loaded in one context and run
// in another (NvAR_Run -> -1). We therefore make the device's primary context current for every
// Maxine call and restore the host's context afterwards. Uses the driver API from nvcuda.dll
// (installed with every NVIDIA driver), so no CUDA toolkit is needed.
class CudaContextGuard {
public:
    CudaContextGuard() {
        if (!Api().ok || !Api().primary) return;
        if (Api().getCurrent) Api().getCurrent(&host_);
        pushed_ = Api().push(Api().primary) == 0;
    }

    // For diagnostics: the host's context at entry and free/total VRAM in MB.
    void* HostContext() const { return host_; }
    static bool MemInfo(size_t& freeMb, size_t& totalMb) {
        size_t f = 0, t = 0;
        if (!Api().ok || !Api().memInfo || Api().memInfo(&f, &t) != 0) return false;
        freeMb = f >> 20;
        totalMb = t >> 20;
        return true;
    }
    ~CudaContextGuard() {
        if (pushed_) {
            void* popped = nullptr;
            Api().pop(&popped);
        }
    }
    CudaContextGuard(const CudaContextGuard&) = delete;
    CudaContextGuard& operator=(const CudaContextGuard&) = delete;

private:
    struct DriverApi {
        using PFN_cuInit = int(__stdcall*)(unsigned);
        using PFN_cuDeviceGet = int(__stdcall*)(int*, int);
        using PFN_cuDevicePrimaryCtxRetain = int(__stdcall*)(void**, int);
        using PFN_cuCtxPushCurrent = int(__stdcall*)(void*);
        using PFN_cuCtxPopCurrent = int(__stdcall*)(void**);
        using PFN_cuCtxGetCurrent = int(__stdcall*)(void**);
        using PFN_cuMemGetInfo = int(__stdcall*)(size_t*, size_t*);
        PFN_cuCtxPushCurrent push = nullptr;
        PFN_cuCtxPopCurrent pop = nullptr;
        PFN_cuCtxGetCurrent getCurrent = nullptr;
        PFN_cuMemGetInfo memInfo = nullptr;
        void* primary = nullptr;
        bool ok = false;

        DriverApi() {
            if (GetEnv("MAXINE_EYECONTACT_NO_CTX_GUARD") == "1") { Log("CUDA context guard disabled"); return; }
            HMODULE cuda = LoadLibraryExA("nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!cuda) { Log("warning: nvcuda.dll not found, CUDA context guard off"); return; }
            auto init = reinterpret_cast<PFN_cuInit>(GetProcAddress(cuda, "cuInit"));
            auto devGet = reinterpret_cast<PFN_cuDeviceGet>(GetProcAddress(cuda, "cuDeviceGet"));
            auto retain = reinterpret_cast<PFN_cuDevicePrimaryCtxRetain>(GetProcAddress(cuda, "cuDevicePrimaryCtxRetain"));
            push = reinterpret_cast<PFN_cuCtxPushCurrent>(GetProcAddress(cuda, "cuCtxPushCurrent_v2"));
            pop = reinterpret_cast<PFN_cuCtxPopCurrent>(GetProcAddress(cuda, "cuCtxPopCurrent_v2"));
            getCurrent = reinterpret_cast<PFN_cuCtxGetCurrent>(GetProcAddress(cuda, "cuCtxGetCurrent"));
            memInfo = reinterpret_cast<PFN_cuMemGetInfo>(GetProcAddress(cuda, "cuMemGetInfo_v2"));
            if (!init || !devGet || !retain || !push || !pop) { Log("warning: CUDA driver API incomplete"); return; }
            int dev = 0, r = 0;
            if ((r = init(0)) != 0 || (r = devGet(&dev, 0)) != 0 || (r = retain(&primary, dev)) != 0) {
                Log("warning: cannot retain CUDA primary context (CUresult %d), guard off", r);
                primary = nullptr;
                return;
            }
            ok = true;
            Log("CUDA context guard on (device 0 primary context)");
        }
    };
    static DriverApi& Api() {
        static DriverApi* api = new DriverApi;  // leaked: never released during DLL unload
        return *api;
    }
    bool pushed_ = false;
    void* host_ = nullptr;
};

// ---------------------------------------------------------------- one loaded feature
// The redirection network is built for one resolution, so we keep one instance per frame size
// (Premiere renders previews at 1/2, 1/4 etc. and we don't want to reload on every switch).
struct FeatureInstance {
    NvAR_FeatureHandle handle = nullptr;
    CUstream stream = nullptr;
    NvCVImage srcGpu, dstGpu, tmp;
    int width = 0, height = 0;
    EngineConfig cfg;
    uint64_t lastUse = 0;

    std::vector<NvAR_Point2f> landmarks, gazeLandmarks;
    std::vector<float> landmarkConf;
    std::vector<NvAR_Rect> boxData;
    NvAR_BBoxes boxes{};
    NvAR_Quaternion headPose{};
    NvAR_Point3f gazeDir[2]{};
    float gaze[2]{};
    float headTrans[3]{};

    ~FeatureInstance() { Release(); }

    void Release() {
        if (handle) { NvAR_Destroy(handle); handle = nullptr; }
        if (stream) { NvAR_CudaStreamDestroy(stream); stream = nullptr; }
        NvCVImage_Dealloc(&srcGpu);
        NvCVImage_Dealloc(&dstGpu);
        NvCVImage_Dealloc(&tmp);
    }

    bool Init(int w, int h, const EngineConfig& c, std::string* err) {
        Release();
        width = w;
        height = h;
        cfg = c;
        const auto t0 = std::chrono::steady_clock::now();

        auto must = [&](NvCV_Status s, const char* what) {
            if (s == NVCV_SUCCESS) return true;
            *err = std::string(what) + " failed: " + StatusStr(s);
            Log("%s", err->c_str());
            return false;
        };
        // Parameters that may not exist in every SDK version: log and continue.
        auto opt = [&](NvCV_Status s, const char* what) {
            if (s != NVCV_SUCCESS) Log("warning: %s -> %s", what, StatusStr(s).c_str());
        };

        if (!must(NvAR_Create(NvAR_Feature_GazeRedirection, &handle), "NvAR_Create(GazeRedirection)")) return false;

        // Model folder: our override, else the installer's NVAR_MODEL_DIR, else <runtime>\models.
        // (Without any of them the SDK fails with -30 "problem deserializing the inference runtime engine".)
        std::string modelDir = GetEnv("MAXINE_EYECONTACT_MODEL_DIR");
        if (modelDir.empty()) modelDir = GetEnv("NVAR_MODEL_DIR");
        if (modelDir.empty() && g_nvARSDKPath) modelDir = std::string(g_nvARSDKPath) + "models";
        Log("model dir: %s", modelDir.empty() ? "(SDK default)" : modelDir.c_str());
        if (!modelDir.empty())
            if (!must(NvAR_SetString(handle, NvAR_Parameter_Config(ModelDir), modelDir.c_str()), "Set ModelDir"))
                return false;

        if (!must(NvAR_CudaStreamCreate(&stream), "NvAR_CudaStreamCreate")) return false;
        if (!must(NvAR_SetCudaStream(handle, NvAR_Parameter_Config(CUDAStream), stream), "Set CUDAStream"))
            return false;

        // Order follows the SDK sample (samples/GazeRedirect/GazeEngine.cpp): config -> Load -> I/O buffers.
        if (!must(NvAR_SetU32(handle, NvAR_Parameter_Config(GazeRedirect), 1u), "Set GazeRedirect")) return false;
        // The sample passes -1 (all bits set) to enable temporal filtering.
        opt(NvAR_SetU32(handle, NvAR_Parameter_Config(Temporal), c.temporal ? 0xFFFFFFFFu : 0u), "Temporal");
        opt(NvAR_SetU32(handle, NvAR_Parameter_Config(Landmarks_Size), 126u), "Landmarks_Size");
        opt(NvAR_SetU32(handle, NvAR_Parameter_Config(UseCudaGraph), 0u), "UseCudaGraph");
        opt(NvAR_SetU32(handle, NvAR_Parameter_Config(EyeSizeSensitivity), (unsigned)c.eyeSizeSensitivity),
            "EyeSizeSensitivity");
        // Newer SDK versions only; older runtimes log a warning and ignore them.
        // MAXINE_EYECONTACT_MINIMAL=1 skips them (diagnostics: configure exactly like the SDK sample).
        if (GetEnv("MAXINE_EYECONTACT_MINIMAL") != "1") {
            opt(NvAR_SetU32(handle, NvAR_Parameter_Config(EnableLookAway), c.lookAway ? 1u : 0u), "EnableLookAway");
            opt(NvAR_SetF32(handle, NvAR_Parameter_Config(GazePitchThresholdLow), c.gazePitchLow), "GazePitchThresholdLow");
            opt(NvAR_SetF32(handle, NvAR_Parameter_Config(GazePitchThresholdHigh), c.gazePitchHigh), "GazePitchThresholdHigh");
            opt(NvAR_SetF32(handle, NvAR_Parameter_Config(GazeYawThresholdLow), c.gazeYawLow), "GazeYawThresholdLow");
            opt(NvAR_SetF32(handle, NvAR_Parameter_Config(GazeYawThresholdHigh), c.gazeYawHigh), "GazeYawThresholdHigh");
            opt(NvAR_SetF32(handle, NvAR_Parameter_Config(HeadPitchThresholdLow), c.headPitchLow), "HeadPitchThresholdLow");
            opt(NvAR_SetF32(handle, NvAR_Parameter_Config(HeadPitchThresholdHigh), c.headPitchHigh), "HeadPitchThresholdHigh");
            opt(NvAR_SetF32(handle, NvAR_Parameter_Config(HeadYawThresholdLow), c.headYawLow), "HeadYawThresholdLow");
            opt(NvAR_SetF32(handle, NvAR_Parameter_Config(HeadYawThresholdHigh), c.headYawHigh), "HeadYawThresholdHigh");
        } else {
            Log("minimal mode: look-away and threshold parameters not set");
        }

        if (!must(NvAR_Load(handle), "NvAR_Load (are the GazeRedirection models installed?)")) return false;

        // GPU images: interleaved 8-bit BGR, as required by the feature.
        if (!must(NvCVImage_Alloc(&srcGpu, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "Alloc input image"))
            return false;
        if (!must(NvAR_SetObject(handle, NvAR_Parameter_Input(Image), &srcGpu, sizeof(NvCVImage)), "Set input Image"))
            return false;
        if (!must(NvCVImage_Alloc(&dstGpu, w, h, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "Alloc output image"))
            return false;
        if (!must(NvAR_SetObject(handle, NvAR_Parameter_Output(Image), &dstGpu, sizeof(NvCVImage)), "Set output Image"))
            return false;
        if (!must(NvAR_SetS32(handle, NvAR_Parameter_Input(Width), w), "Set input Width")) return false;
        if (!must(NvAR_SetS32(handle, NvAR_Parameter_Input(Height), h), "Set input Height")) return false;

        // Auxiliary outputs. We don't use them, but the sample always provides them, so we do too.
        unsigned numKpts = 126;
        opt(NvAR_GetU32(handle, NvAR_Parameter_Config(Landmarks_Size), &numKpts), "Get Landmarks_Size");
        if (numKpts == 0 || numKpts > 1024) numKpts = 126;
        landmarks.assign(numKpts, NvAR_Point2f{});
        gazeLandmarks.assign(12, NvAR_Point2f{});  // sample: num_output_landmarks = 12
        landmarkConf.assign(numKpts, 0.f);
        boxData.assign(1, NvAR_Rect{});            // single face (batch size 1)
        boxes.boxes = boxData.data();
        boxes.max_boxes = 1;
        boxes.num_boxes = 1;
        if (!must(NvAR_SetObject(handle, NvAR_Parameter_Output(Landmarks), landmarks.data(), sizeof(NvAR_Point2f)),
                  "Set Landmarks"))
            return false;
        opt(NvAR_SetObject(handle, NvAR_Parameter_Output(GazeOutputLandmarks), gazeLandmarks.data(), sizeof(NvAR_Point2f)),
            "Out GazeOutputLandmarks");
        opt(NvAR_SetF32Array(handle, NvAR_Parameter_Output(LandmarksConfidence), landmarkConf.data(), (int)landmarkConf.size()),
            "Out LandmarksConfidence");
        opt(NvAR_SetF32Array(handle, NvAR_Parameter_Output(OutputGazeVector), gaze, 2), "Out OutputGazeVector");
        opt(NvAR_SetF32Array(handle, NvAR_Parameter_Output(OutputHeadTranslation), headTrans, 3), "Out OutputHeadTranslation");
        opt(NvAR_SetObject(handle, NvAR_Parameter_Output(HeadPose), &headPose, sizeof(NvAR_Quaternion)), "Out HeadPose");
        opt(NvAR_SetObject(handle, NvAR_Parameter_Output(GazeDirection), gazeDir, sizeof(NvAR_Point3f)), "Out GazeDirection");
        opt(NvAR_SetObject(handle, NvAR_Parameter_Output(BoundingBoxes), &boxes, sizeof(NvAR_BBoxes)), "Out BoundingBoxes");

        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Log("GazeRedirection loaded for %dx%d in %.0f ms (temporal=%d, eyeSize=%d, lookAway=%d)", w, h, ms,
            (int)c.temporal, c.eyeSizeSensitivity, (int)c.lookAway);
        return true;
    }

    bool Run(uint8_t* bgr, std::string* err, void* hostCtx) {
        const bool first = (runSuccesses + runFailures) == 0;
        if (first) {
            Log("first frame %dx%d: %s, host CUDA ctx=%p, thread=%lu", width, height,
                FrameStats(bgr, width, height).c_str(), hostCtx, GetCurrentThreadId());
            if (DebugDumps()) DumpBgr("first", bgr, width, height);
        }

        NvCVImage cpu;  // wraps the caller's buffer, does not own it
        NvCVImage_Init(&cpu, width, height, width * 3, bgr, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);

        NvCV_Status s = NvCVImage_Transfer(&cpu, &srcGpu, 1.f, stream, &tmp);
        if (s != NVCV_SUCCESS) { *err = "Upload failed: " + StatusStr(s); Log("%s", err->c_str()); return false; }

        s = NvAR_Run(handle);
        // AR SDK 0.8.7 bug: with Temporal on, once a frame without a face has been seen, every later frame
        // with a face fails with -1 (state gets stuck). Reloading the feature clears it (~0.1 s), so reset
        // and retry this frame once. Happens on cutaways, "Media pending" placeholders, people turning away.
        if (s != NVCV_SUCCESS && cfg.temporal) {
            std::string reinitErr;
            const EngineConfig c = cfg;
            if (Init(width, height, c, &reinitErr) &&
                NvCVImage_Transfer(&cpu, &srcGpu, 1.f, stream, &tmp) == NVCV_SUCCESS) {
                s = NvAR_Run(handle);
                ++temporalResets;
                if (temporalResets <= 5 || temporalResets % 100 == 0)
                    Log("temporal state reset #%llu -> retry %s", temporalResets, s == NVCV_SUCCESS ? "ok" : "failed");
            }
        }
        if (s != NVCV_SUCCESS) {
            *err = "NvAR_Run failed: " + StatusStr(s);
            // Rate-limited: first 5 failures, then every 100th.
            if (++runFailures <= 5 || runFailures % 100 == 0) {
                size_t freeMb = 0, totalMb = 0;
                CudaContextGuard::MemInfo(freeMb, totalMb);
                Log("%s [%dx%d, failure #%llu, successes %llu] VRAM free %zu/%zu MB, host ctx=%p, thread=%lu, %s",
                    err->c_str(), width, height, runFailures, runSuccesses, freeMb, totalMb, hostCtx,
                    GetCurrentThreadId(), FrameStats(bgr, width, height).c_str());
                if (runFailures == 1 && DebugDumps()) DumpBgr("fail", bgr, width, height);
            }
            return false;
        }
        if (++runSuccesses == 1 || runSuccesses % 500 == 0) {
            float conf = 0.f;
            for (float v : landmarkConf) conf += v;
            if (!landmarkConf.empty()) conf /= static_cast<float>(landmarkConf.size());
            Log("NvAR_Run ok [%dx%d, success #%llu]: faces=%u box=(%.0f,%.0f %.0fx%.0f) landmark conf=%.2f gaze=(%.1f,%.1f) deg",
                width, height, runSuccesses, (unsigned)boxes.num_boxes, boxData[0].x, boxData[0].y, boxData[0].width,
                boxData[0].height, conf, gaze[0] * 57.2958f, gaze[1] * 57.2958f);
        }

        // Download into pageable host memory: this copy completes before returning.
        s = NvCVImage_Transfer(&dstGpu, &cpu, 1.f, stream, &tmp);
        if (s != NVCV_SUCCESS) { *err = "Download failed: " + StatusStr(s); Log("%s", err->c_str()); return false; }
        return true;
    }

    unsigned long long runFailures = 0, runSuccesses = 0, temporalResets = 0;
};

constexpr size_t kMaxInstances = 3;

}  // namespace

// ---------------------------------------------------------------- engine
struct MaxineEngine::Impl {
    std::mutex mtx;
    std::vector<std::unique_ptr<FeatureInstance>> instances;
    uint64_t useCounter = 0;

    // Remember the last failure so we don't retry a multi-second load on every frame.
    bool failed = false;
    int failedW = 0, failedH = 0;
    EngineConfig failedCfg;
    std::string failedMsg;
};

MaxineEngine::MaxineEngine() : p_(new Impl) {}

void EyeContactLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

MaxineEngine& MaxineEngine::Instance() {
    static MaxineEngine* e = new MaxineEngine;  // leaked on purpose
    return *e;
}

bool MaxineEngine::Process(uint8_t* bgr, int width, int height, const EngineConfig& cfg, std::string* error) {
    std::lock_guard<std::mutex> lock(p_->mtx);
    Impl& m = *p_;
    std::string localErr;
    std::string& err = error ? *error : localErr;
    err.clear();

    // Keep the runtime folder in the DLL search path for the duration of every Maxine call (anything it
    // still loads lazily is found there), and restore the default search order afterwards so we don't
    // change how Premiere itself loads DLLs.
    struct ScopedDllDirectory {
        ScopedDllDirectory() { if (g_nvARSDKPath) SetDllDirectoryA(g_nvARSDKPath); }
        ~ScopedDllDirectory() { if (g_nvARSDKPath) SetDllDirectoryA(nullptr); }
    } scopedDllDirectory;

    // Every Maxine call below runs in our own CUDA context, not the host's (see CudaContextGuard).
    CudaContextGuard cudaContext;

    if (width < 16 || height < 16) return false;

    // Settings changed -> every cached instance is stale.
    for (auto& inst : m.instances)
        if (!(inst->cfg == cfg)) inst.reset();
    std::erase_if(m.instances, [](const auto& p) { return !p; });

    FeatureInstance* inst = nullptr;
    for (auto& i : m.instances)
        if (i->width == width && i->height == height) inst = i.get();

    if (!inst) {
        if (m.failed && m.failedW == width && m.failedH == height && m.failedCfg == cfg) {
            err = m.failedMsg;
            return false;
        }
        if (m.instances.size() >= kMaxInstances) {  // evict least recently used
            auto lru = std::min_element(m.instances.begin(), m.instances.end(),
                                        [](const auto& a, const auto& b) { return a->lastUse < b->lastUse; });
            m.instances.erase(lru);
        }
        auto fresh = std::make_unique<FeatureInstance>();
        if (!PreloadRuntime(&err) || !fresh->Init(width, height, cfg, &err)) {
            m.failed = true;
            m.failedW = width;
            m.failedH = height;
            m.failedCfg = cfg;
            m.failedMsg = err;
            return false;
        }
        m.failed = false;
        inst = fresh.get();
        m.instances.push_back(std::move(fresh));
    }

    inst->lastUse = ++m.useCounter;
    return inst->Run(bgr, &err, cudaContext.HostContext());
}

void MaxineEngine::Shutdown() {
    std::lock_guard<std::mutex> lock(p_->mtx);
    CudaContextGuard cudaContext;  // destroy the features in the context they were created in
    p_->instances.clear();
    if (g_nvARSDKPath) SetDllDirectoryA(nullptr);
    p_->failed = false;
    Log("Engine shut down");
}
