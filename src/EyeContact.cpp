// EyeContact - eye contact correction effect for Premiere Pro and After Effects,
// built on the NVIDIA Maxine AR SDK (GazeRedirection feature).
//
// Classic (non-SmartFX) AE effect API, which Premiere Pro also hosts.
//   After Effects : 8-bit and 16-bit ARGB.
//   Premiere Pro  : native BGRA 8-bit and BGRA 32-bit float (via PF_PixelFormatSuite).

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "SPTypes.h"   // defines SPAPI, needed by PrSDKAESupport.h
#include "SPBasic.h"
#include "PrSDKAESupport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "ColorUtil.h"
#include "MaxineEngine.h"

// Must match AE_Effect_Version in EyeContactPiPL.r: PF_VERSION(1,0,0,DEVELOP,1) == 524289
#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define BUG_VERSION   0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

#define EFFECT_NAME       "EyeContact"
#define EFFECT_MATCH_NAME "Maxine EyeContact"
#define EFFECT_CATEGORY   "Maxine"

// Parameter order (index into params[]).
enum {
    P_INPUT = 0,
    P_STRENGTH,
    P_KEEPCOLOR,
    P_TEMPORAL,
    P_EYESIZE,
    P_LOOKAWAY,
    P_TOPIC_START,
    P_GAZE_PITCH_LOW,
    P_GAZE_PITCH_HIGH,
    P_GAZE_YAW_LOW,
    P_GAZE_YAW_HIGH,
    P_HEAD_PITCH_LOW,
    P_HEAD_PITCH_HIGH,
    P_HEAD_YAW_LOW,
    P_HEAD_YAW_HIGH,
    P_TOPIC_END,
    P_NUM
};

// Stable disk IDs - never renumber, saved projects depend on them.
enum {
    ID_STRENGTH = 1,
    ID_TEMPORAL,
    ID_EYESIZE,
    ID_LOOKAWAY,
    ID_TOPIC_START,
    ID_GAZE_PITCH_LOW,
    ID_GAZE_PITCH_HIGH,
    ID_GAZE_YAW_LOW,
    ID_GAZE_YAW_HIGH,
    ID_HEAD_PITCH_LOW,
    ID_HEAD_PITCH_HIGH,
    ID_HEAD_YAW_LOW,
    ID_HEAD_YAW_HIGH,
    ID_TOPIC_END,
    ID_KEEPCOLOR
};

namespace {

enum class Fmt { ARGB8, ARGB16, BGRA8, BGRA32F };

struct PixBGRA8  { uint8_t b, g, r, a; };
struct PixBGRA32 { float b, g, r, a; };

constexpr bool IsPremiere(const PF_InData* in) { return in->appl_id == 'PrMr'; }

int BytesPerPixel(Fmt f) {
    switch (f) {
        case Fmt::ARGB16:  return 8;
        case Fmt::BGRA32F: return 16;
        default:           return 4;
    }
}

// Premiere may hand us bottom-up worlds with negative rowbytes, so keep the arithmetic signed.
inline char* Row(const PF_EffectWorld* w, int y) {
    return reinterpret_cast<char*>(w->data) + static_cast<intptr_t>(y) * w->rowbytes;
}

// Premiere-only suite; returns nullptr in After Effects. Release with ReleasePixelFormatSuite.
const PF_PixelFormatSuite1* AcquirePixelFormatSuite(PF_InData* in_data) {
    if (!IsPremiere(in_data) || !in_data->pica_basicP) return nullptr;
    const void* raw = nullptr;
    if (in_data->pica_basicP->AcquireSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, &raw) != 0) return nullptr;
    return static_cast<const PF_PixelFormatSuite1*>(raw);
}

void ReleasePixelFormatSuite(PF_InData* in_data) {
    in_data->pica_basicP->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
}

Fmt DetectFormat(PF_InData* in_data, PF_EffectWorld* world) {
    {
        const PF_PixelFormatSuite1* suite = AcquirePixelFormatSuite(in_data);
        if (suite) {
            PrPixelFormat pf = PrPixelFormat_BGRA_4444_8u;
            suite->GetPixelFormat(world, &pf);
            ReleasePixelFormatSuite(in_data);
            if (pf == PrPixelFormat_BGRA_4444_32f) return Fmt::BGRA32F;
            if (pf == PrPixelFormat_BGRA_4444_8u)  return Fmt::BGRA8;
        }
    }
    return PF_WORLD_IS_DEEP(world) ? Fmt::ARGB16 : Fmt::ARGB8;
}

inline uint8_t To8(float v01) { return static_cast<uint8_t>(std::clamp(v01, 0.f, 1.f) * 255.f + 0.5f); }

// world -> packed BGR8
void ReadBGR(const PF_EffectWorld* w, Fmt f, uint8_t* dst) {
    const int W = w->width, H = w->height;
    for (int y = 0; y < H; ++y) {
        const char* row = Row(w, y);
        uint8_t* d = dst + static_cast<size_t>(y) * W * 3;
        switch (f) {
            case Fmt::ARGB8: {
                auto p = reinterpret_cast<const PF_Pixel8*>(row);
                for (int x = 0; x < W; ++x, d += 3) { d[0] = p[x].blue; d[1] = p[x].green; d[2] = p[x].red; }
            } break;
            case Fmt::ARGB16: {
                auto p = reinterpret_cast<const PF_Pixel16*>(row);
                constexpr float k = 1.f / PF_MAX_CHAN16;
                for (int x = 0; x < W; ++x, d += 3) {
                    d[0] = To8(p[x].blue * k); d[1] = To8(p[x].green * k); d[2] = To8(p[x].red * k);
                }
            } break;
            case Fmt::BGRA8: {
                auto p = reinterpret_cast<const PixBGRA8*>(row);
                for (int x = 0; x < W; ++x, d += 3) { d[0] = p[x].b; d[1] = p[x].g; d[2] = p[x].r; }
            } break;
            case Fmt::BGRA32F: {
                auto p = reinterpret_cast<const PixBGRA32*>(row);
                for (int x = 0; x < W; ++x, d += 3) { d[0] = To8(p[x].b); d[1] = To8(p[x].g); d[2] = To8(p[x].r); }
            } break;
        }
    }
}

// Writes input to output, replacing only the pixels Maxine actually changed (the eye regions),
// mixed by `amount`. Untouched pixels keep their full original precision (16-bit / float / HDR).
void WriteResult(const PF_EffectWorld* in, PF_EffectWorld* out, Fmt f, const uint8_t* orig, const uint8_t* proc,
                 float amount, bool keepColor) {
    const int W = out->width, H = out->height;

    // New value for one changed pixel, in the source's channel units. k converts Maxine's 0..255.
    auto blend = [amount, keepColor](float sb, float sg, float sr, const uint8_t* q, float k, float& b, float& g,
                                     float& r) {
        b = q[0] * k; g = q[1] * k; r = q[2] * k;
        if (keepColor) KeepSourceColor(sb, sg, sr, b, g, r);
        b = sb + amount * (b - sb);
        g = sg + amount * (g - sg);
        r = sr + amount * (r - sr);
    };
    auto c8  = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.f, 255.f) + 0.5f); };
    auto c16 = [](float v) { return static_cast<A_u_short>(std::clamp(v, 0.f, static_cast<float>(PF_MAX_CHAN16)) + 0.5f); };

    for (int y = 0; y < H; ++y) {
        const char* srow = Row(in, y);
        char* drow = Row(out, y);
        const uint8_t* o = orig + static_cast<size_t>(y) * W * 3;
        const uint8_t* q = proc + static_cast<size_t>(y) * W * 3;
        float b, g, r;

        switch (f) {
            case Fmt::ARGB8: {
                auto s = reinterpret_cast<const PF_Pixel8*>(srow);
                auto d = reinterpret_cast<PF_Pixel8*>(drow);
                for (int x = 0; x < W; ++x, o += 3, q += 3) {
                    d[x] = s[x];
                    if (o[0] != q[0] || o[1] != q[1] || o[2] != q[2]) {
                        blend(s[x].blue, s[x].green, s[x].red, q, 1.f, b, g, r);
                        d[x].blue = c8(b); d[x].green = c8(g); d[x].red = c8(r);
                    }
                }
            } break;
            case Fmt::ARGB16: {
                auto s = reinterpret_cast<const PF_Pixel16*>(srow);
                auto d = reinterpret_cast<PF_Pixel16*>(drow);
                constexpr float k = PF_MAX_CHAN16 / 255.f;
                for (int x = 0; x < W; ++x, o += 3, q += 3) {
                    d[x] = s[x];
                    if (o[0] != q[0] || o[1] != q[1] || o[2] != q[2]) {
                        blend(s[x].blue, s[x].green, s[x].red, q, k, b, g, r);
                        d[x].blue = c16(b); d[x].green = c16(g); d[x].red = c16(r);
                    }
                }
            } break;
            case Fmt::BGRA8: {
                auto s = reinterpret_cast<const PixBGRA8*>(srow);
                auto d = reinterpret_cast<PixBGRA8*>(drow);
                for (int x = 0; x < W; ++x, o += 3, q += 3) {
                    d[x] = s[x];
                    if (o[0] != q[0] || o[1] != q[1] || o[2] != q[2]) {
                        blend(s[x].b, s[x].g, s[x].r, q, 1.f, b, g, r);
                        d[x].b = c8(b); d[x].g = c8(g); d[x].r = c8(r);
                    }
                }
            } break;
            case Fmt::BGRA32F: {
                auto s = reinterpret_cast<const PixBGRA32*>(srow);
                auto d = reinterpret_cast<PixBGRA32*>(drow);
                for (int x = 0; x < W; ++x, o += 3, q += 3) {
                    d[x] = s[x];
                    if (o[0] != q[0] || o[1] != q[1] || o[2] != q[2]) {
                        blend(s[x].b, s[x].g, s[x].r, q, 1.f / 255.f, b, g, r);
                        d[x].b = b; d[x].g = g; d[x].r = r;
                    }
                }
            } break;
        }
    }
}

void CopyWorld(const PF_EffectWorld* in, PF_EffectWorld* out, Fmt f) {
    const int W = std::min(in->width, out->width);
    const int H = std::min(in->height, out->height);
    const size_t bytes = static_cast<size_t>(W) * BytesPerPixel(f);
    if (in->data == out->data) return;
    for (int y = 0; y < H; ++y) std::memcpy(Row(out, y), Row(in, y), bytes);
}

void ReportErrorOnce(PF_OutData* out_data, const std::string& msg) {
    static std::atomic<bool> reported{false};
    if (msg.empty() || reported.exchange(true)) return;
    snprintf(out_data->return_msg, sizeof(out_data->return_msg),
             "EyeContact: %s\rDetails: %%TEMP%%\\EyeContact.log", msg.c_str());
    out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
}

EngineConfig ReadConfig(PF_ParamDef* params[]) {
    auto f = [&](int i) { return static_cast<float>(params[i]->u.fs_d.value); };
    // "High" must be above "Low", otherwise the SDK rejects the value.
    auto hi = [](float lo, float h) { return std::min(35.f, std::max(h, lo + 1.f)); };

    EngineConfig c;
    c.temporal           = params[P_TEMPORAL]->u.bd.value != 0;
    c.eyeSizeSensitivity = std::clamp<int>(params[P_EYESIZE]->u.sd.value, 2, 5);
    c.lookAway           = params[P_LOOKAWAY]->u.bd.value != 0;
    c.gazePitchLow  = f(P_GAZE_PITCH_LOW);  c.gazePitchHigh = hi(c.gazePitchLow, f(P_GAZE_PITCH_HIGH));
    c.gazeYawLow    = f(P_GAZE_YAW_LOW);    c.gazeYawHigh   = hi(c.gazeYawLow,   f(P_GAZE_YAW_HIGH));
    c.headPitchLow  = f(P_HEAD_PITCH_LOW);  c.headPitchHigh = hi(c.headPitchLow, f(P_HEAD_PITCH_HIGH));
    c.headYawLow    = f(P_HEAD_YAW_LOW);    c.headYawHigh   = hi(c.headYawLow,   f(P_HEAD_YAW_HIGH));
    return c;
}

// ---------------------------------------------------------------- commands

PF_Err About(PF_InData*, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    snprintf(out_data->return_msg, sizeof(out_data->return_msg),
             "%s v%d.%d\rEye contact correction powered by the NVIDIA Maxine AR SDK (Gaze Redirection).\r"
             "Requires an NVIDIA RTX GPU.",
             EFFECT_NAME, MAJOR_VERSION, MINOR_VERSION);
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);
    // Must match AE_Effect_Global_OutFlags / _2 in EyeContactPiPL.r.
    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE;                // 0x02000000
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING;    // 0x08000000

    if (const PF_PixelFormatSuite1* suite = AcquirePixelFormatSuite(in_data)) {
        suite->ClearSupportedPixelFormats(in_data->effect_ref);
        suite->AddSupportedPixelFormat(in_data->effect_ref, PrPixelFormat_BGRA_4444_32f);
        suite->AddSupportedPixelFormat(in_data->effect_ref, PrPixelFormat_BGRA_4444_8u);
        ReleasePixelFormatSuite(in_data);
    }
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Strength", 0, 100, 0, 100, 100, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT, 0,
                         ID_STRENGTH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Keep original color", TRUE, 0, ID_KEEPCOLOR);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Temporal smoothing", TRUE, 0, ID_TEMPORAL);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Eye size sensitivity", 2, 5, 2, 5, 3, ID_EYESIZE);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Natural look-away", FALSE, 0, ID_LOOKAWAY);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Angle thresholds (deg)", PF_ParamFlag_START_COLLAPSED, ID_TOPIC_START);

    struct { const char* name; float dflt; int id; } th[] = {
        {"Gaze pitch: always redirect", 20.f, ID_GAZE_PITCH_LOW},
        {"Gaze pitch: give up above",   30.f, ID_GAZE_PITCH_HIGH},
        {"Gaze yaw: always redirect",   20.f, ID_GAZE_YAW_LOW},
        {"Gaze yaw: give up above",     30.f, ID_GAZE_YAW_HIGH},
        {"Head pitch: always redirect", 15.f, ID_HEAD_PITCH_LOW},
        {"Head pitch: give up above",   25.f, ID_HEAD_PITCH_HIGH},
        {"Head yaw: always redirect",   25.f, ID_HEAD_YAW_LOW},
        {"Head yaw: give up above",     30.f, ID_HEAD_YAW_HIGH},
    };
    for (const auto& t : th) {
        AEFX_CLR_STRUCT(def);
        PF_ADD_FLOAT_SLIDERX(t.name, 10, 35, 10, 35, t.dflt, PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, 0, t.id);
    }

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(ID_TOPIC_END);

    out_data->num_params = P_NUM;
    return err;
}

PF_Err Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
    PF_EffectWorld* input = &params[P_INPUT]->u.ld;
    const Fmt fmt = DetectFormat(in_data, output);
    const float amount = static_cast<float>(params[P_STRENGTH]->u.fs_d.value) / 100.f;

    {   // Diagnostics: log each pixel format / world layout once per session.
        static std::atomic<unsigned> seen{0};
        const unsigned bit = 1u << static_cast<unsigned>(fmt);
        if (!(seen.fetch_or(bit) & bit)) {
            static const char* names[] = {"ARGB8", "ARGB16", "BGRA8", "BGRA32F"};
            EyeContactLog("host=%s format=%s in %dx%d rowbytes=%d, out %dx%d rowbytes=%d",
                          IsPremiere(in_data) ? "Premiere" : "AE", names[static_cast<int>(fmt)], input->width,
                          input->height, input->rowbytes, output->width, output->height, output->rowbytes);
        }
    }

    if (amount <= 0.f || input->width != output->width || input->height != output->height) {
        CopyWorld(input, output, fmt);
        return PF_Err_NONE;
    }

    const int w = output->width, h = output->height;
    thread_local std::vector<uint8_t> orig, proc;
    orig.resize(static_cast<size_t>(w) * h * 3);
    ReadBGR(input, fmt, orig.data());
    proc = orig;

    std::string err;
    if (!MaxineEngine::Instance().Process(proc.data(), w, h, ReadConfig(params), &err)) {
        CopyWorld(input, output, fmt);  // fail safe: never break the user's render
        ReportErrorOnce(out_data, err);
        return PF_Err_NONE;
    }

    WriteResult(input, output, fmt, orig.data(), proc.data(), std::min(amount, 1.f),
                params[P_KEEPCOLOR]->u.bd.value != 0);
    return PF_Err_NONE;
}

}  // namespace

// ---------------------------------------------------------------- entry points

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                     const char* inHostVersion) {
    PF_Err result = PF_Err_INVALID_CALLBACK;
    PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, EFFECT_NAME, EFFECT_MATCH_NAME, EFFECT_CATEGORY,
                            AE_RESERVED_INFO, "EffectMain", "https://developer.nvidia.com/maxine");
    return result;
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra) {
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:          err = About(in_data, out_data, params, output); break;
            case PF_Cmd_GLOBAL_SETUP:   err = GlobalSetup(in_data, out_data, params, output); break;
            case PF_Cmd_GLOBAL_SETDOWN: MaxineEngine::Instance().Shutdown(); break;
            case PF_Cmd_PARAMS_SETUP:   err = ParamsSetup(in_data, out_data, params, output); break;
            case PF_Cmd_RENDER:         err = Render(in_data, out_data, params, output); break;
            default: break;
        }
    } catch (const std::bad_alloc&) {
        err = PF_Err_OUT_OF_MEMORY;
    } catch (PF_Err thrown) {
        err = thrown;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
