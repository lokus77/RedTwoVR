// OpenXR (Windows, D3D12): the game's picture on a virtual screen in a VR headset.
//
// Stereo 3D (the default; R2_XR_MODE=screen keeps the flat screen): Aurora's EFB holds both
// eyes side by side (aurora::gfx::set_stereo). At the start of each frame frameViews locates
// the eyes for the time the frame will be shown (the last frame's predicted display time plus
// the frames in flight) and gx_bridge sends their views (GX_AURORA_SET_STEREO_VIEWS): every
// perspective draw then takes each eye's frustum, its position (the game's camera at the
// origin of the LOCAL space; R2_XR_SCALE game units per metre) and the head's rotation, and
// keeps the game's own depth. The present hook gets the left and the right eye's XFBs and
// submits them as a projection layer with the very poses they were drawn with, so the runtime
// corrects what moved since. The window shows the left eye.
//
// The flat screen: the frame the window shows (Aurora's XFB at the internal resolution) on a
// screen floating in front of the player (a quad layer, which the runtime composites at the
// headset's rate, so turning the head stays smooth whatever the game's frame rate). The window
// keeps showing the game as before. While the launcher or the F1 menu is up (ImGui, drawn over
// the window only) the headset shows the window's picture that way, in either mode.
//
// The session runs on Aurora's own D3D12 device and queue (Dawn's): preinit asks OpenXR for
// the headset's adapter before Aurora picks one (aurora::gfx::set_adapter_luid). After each
// presented frame (aurora::gfx::set_present_hook, on Aurora's render worker) the picture is
// drawn into a texture of ours (an ID3D12Resource imported into Dawn), which a D3D12 copy puts
// into the swapchain image: the runtime's images may be typeless, which Dawn cannot import,
// and a copy between formats of one family keeps the sRGB swapchain's bits as they are (the
// game's picture is sRGB-encoded already, so the runtime shows it as the window does).
// xrWaitFrame paces the render worker at the headset's rate when the game draws faster.
//
// The Touch controllers (OpenXR actions) drive a virtual SDL gamepad laid out like an Xbox
// controller, so the game, the menus and the hotkeys take them as any other controller (port 1
// follows the one in use; the left grip held + the left thumbstick = the d-pad): thumbsticks =
// control stick / C-stick, triggers = L / R (analog),
// grips = LB / RB (RB = Z), A B X Y = A B X Y, the left menu button = Start, the left
// thumbstick's click = Back (the F1 menu, the hotkeys), the right one's = the right stick click.
//
// --xr or R2_XR=1 turns it on. R2_XR_SCREEN=<width m>,<distance m> sizes the screen
// (default 3.2 m wide, 2.5 m away: about 65 degrees across).
#include "gfx/xr.h"

#ifndef R2_OPENXR

namespace rt::xr {
bool preinit(int, char**) { return false; }
void start() {}
bool active() { return false; }
bool enabled() { return false; }
int recommendedEyeHeight() { return 0; }
bool stereo() { return false; }
bool frameViews(float (*)[16]) { return false; }
void setFlight(bool) {}
bool cameraRotation(float*) { return false; }
void setGameView(float, float) {}
void applySettings() {}
void resetCameraZoom() {}
void haptic(float, float) {}
}  // namespace rt::xr

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include <dawn/native/D3D12Backend.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>
#include <vector>

#include "runtime.h"
#include "settings.h"

using Microsoft::WRL::ComPtr;

namespace rt::xr {
namespace {

XrInstance s_instance = XR_NULL_HANDLE;
XrSystemId s_system = XR_NULL_SYSTEM_ID;
XrSession s_session = XR_NULL_HANDLE;
XrSpace s_space = XR_NULL_HANDLE;
XrSessionState s_state = XR_SESSION_STATE_UNKNOWN;
std::atomic_bool s_running = false;
bool s_lost = false;  // instance or session gone: nothing more this run

ComPtr<ID3D12Device> s_d3d;
ComPtr<ID3D12CommandQueue> s_queue;

// the screen: swapchain, our texture (drawn by Dawn, copied into the swapchain image)
XrSwapchain s_swapchain = XR_NULL_HANDLE;
std::vector<XrSwapchainImageD3D12KHR> s_images;
DXGI_FORMAT s_swapFormat = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT s_drawFormat = DXGI_FORMAT_UNKNOWN;  // our texture's (the same family, not sRGB)
wgpu::TextureFormat s_drawWgpu = wgpu::TextureFormat::Undefined;
uint32_t s_width = 0, s_height = 0;
ComPtr<ID3D12Resource> s_drawRes;
wgpu::SharedTextureMemory s_drawMem;
wgpu::Texture s_drawTex;
wgpu::RenderPipeline s_pipeline;
wgpu::Sampler s_sampler;
// the 3D screen: the screen drawn in each eye's view (a projection layer: per-eye quad layers
// showed the right eye's picture to both eyes on Virtual Desktop)
wgpu::RenderPipeline s_screenPipeline;
wgpu::Buffer s_cornerBuf[2];
uint32_t s_eyeSize = 0;  // the projection layer's per-eye size (square)

// the copy: our own command lists on Dawn's queue
constexpr int kSlots = 3;
std::array<ComPtr<ID3D12CommandAllocator>, kSlots> s_alloc;
std::array<UINT64, kSlots> s_slotValue{};
ComPtr<ID3D12GraphicsCommandList> s_list;
ComPtr<ID3D12Fence> s_fence;
UINT64 s_fenceValue = 0;
HANDLE s_fenceEvent = nullptr;
u64 s_frame = 0;

float s_screenWidth = 3.2f, s_screenDistance = 2.5f;

// stereo: the views each frame was drawn with (frameViews), by Aurora's frame number
bool s_stereo = false;
// game units per metre (setting xr_world_scale, R2_XR_SCALE) and the eyes' distance ahead of
// the game's camera in metres (xr_camera_forward); the controllers change them in flight
std::atomic<float> s_worldScale{1.f};
std::atomic<float> s_cameraForward{0.f};
std::atomic<float> s_screenDepth{2.f};  // the 3D screen's eye distance, times the real one
std::atomic_bool s_flight = false;  // the player is flying (setFlight)
// The game's cockpit view, followed from the controllers (the game keeps it to itself): X (left
// controller) switches it on / off, and the targeting computer (Y) shows in it too. Each flight
// starts outside. In it the eyes stay at the game's camera (no xr_camera_forward: it would put
// them out through the canopy).
std::atomic_bool s_cockpit = false, s_targeting = false;
bool cockpitView() { return s_cockpit || s_targeting; }
// the head's orientation the game's camera took (cameraRotation; the game's thread)
XrQuaternionf s_camQ{0.f, 0.f, 0.f, 1.f};
bool s_camValid = false;
// Tests without a headset (R2_XR_FAKE=1): stereo with made-up eyes (R2_XR_FAKE_YAW=<degrees>, R2_XR_FAKE_PITCH:
// the head turned left), the window showing the left eye
bool s_fake = false;
std::atomic_bool s_tuneSave = false;  // changed: the game thread saves them (frameViews)
bool s_tuning = false;
std::atomic<XrTime> s_displayTime{0};      // the last predicted display time (xrWaitFrame)
// The headset's refresh rate (XR_FB_display_refresh_rate, if the runtime has it): setting
// xr_refresh_rate, asked for when the session runs and when the menu changes it (render worker)
PFN_xrEnumerateDisplayRefreshRatesFB s_enumRates = nullptr;
PFN_xrRequestDisplayRefreshRateFB s_requestRate = nullptr;
PFN_xrGetDisplayRefreshRateFB s_getRate = nullptr;
std::atomic_bool s_rateDirty = false;
std::atomic<XrDuration> s_displayPeriod{0};
struct FrameViews {
    uint32_t frame = 0;
    bool valid = false;
    bool screen = false;  // a 3D screen (not flying): the game's own view seen from each eye
    XrView views[2]{};
};
std::mutex s_viewsMutex;
std::array<FrameViews, 8> s_frameViews{};  // ring by frame number

// the controllers: actions, and the virtual gamepad they drive
XrActionSet s_actionSet = XR_NULL_HANDLE;
enum Act { TrigL, TrigR, GripL, GripR, StickL, StickR, BtnA, BtnB, BtnX, BtnY, BtnMenu, ClickL, ClickR, kActs };
std::array<XrAction, kActs> s_act{};
XrAction s_haptic = XR_NULL_HANDLE;  // both controllers' vibration (haptic)
std::mutex s_hapticMutex;
float s_hapticAmp = 0.f, s_hapticSecs = 0.f;  // the pulse to start at the next updatePad
float s_hapticPlaying = 0.f;  // the amplitude of the pulse playing, until s_hapticUntil
std::chrono::steady_clock::time_point s_hapticUntil{};
SDL_Joystick* s_joy = nullptr;
bool s_padLive = false;  // the virtual gamepad holds what the controllers gave (not neutral)

bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "";
    if (s_instance) xrResultToString(s_instance, r, name);
    logf("xr", "%s failed: %s (%d)", what, name[0] ? name : "?", int(r));
    if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST) s_lost = true;
    return false;
}

// --xr, R2_XR=1 or the setting vr (--no-xr: not this run)
bool asked(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--xr")) return true;
        if (!strcmp(argv[i], "--no-xr")) return false;
    }
    return envFlag("R2_XR") || settings::get().vr;
}

// ---------------------------------------------------------------------------------------
// session state
// ---------------------------------------------------------------------------------------
// the rate setting xr_refresh_rate asks for (0: the runtime's own), the nearest one it offers
void applyRefreshRate() {
    if (!s_enumRates || !s_requestRate || !s_session) return;
    uint32_t n = 0;
    if (!ok(s_enumRates(s_session, 0, &n, nullptr), "xrEnumerateDisplayRefreshRatesFB") || n == 0) return;
    std::vector<float> rates(n);
    if (!ok(s_enumRates(s_session, n, &n, rates.data()), "xrEnumerateDisplayRefreshRatesFB")) return;
    std::string list;
    for (float r : rates) list += (list.empty() ? "" : ", ") + std::to_string(int(std::lround(r)));
    float now = 0.f;
    if (s_getRate) s_getRate(s_session, &now);
    const int want = settings::get().xrRefreshRate;
    if (want <= 0) {
        logf("xr", "refresh rates offered: %s Hz; %g Hz now (the runtime's own)", list.c_str(), now);
        return;
    }
    float best = rates[0];
    for (float r : rates)
        if (std::abs(r - float(want)) < std::abs(best - float(want))) best = r;
    if (ok(s_requestRate(s_session, best), "xrRequestDisplayRefreshRateFB"))
        logf("xr", "refresh rates offered: %s Hz; asked for %g Hz (setting %d, was %g)", list.c_str(), best, want, now);
}

void pollEvents() {
    if (s_rateDirty.exchange(false) && s_running) applyRefreshRate();
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (s_instance && xrPollEvent(s_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            logf("xr", "the OpenXR runtime is going away");
            s_running = false;
            s_lost = true;
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& e = reinterpret_cast<const XrEventDataSessionStateChanged&>(ev);
            s_state = e.state;
            logf("xr", "session state %d", int(e.state));
            if (e.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (ok(xrBeginSession(s_session, &bi), "xrBeginSession")) {
                    s_running = true;
                    logf("xr", "session running: the game is on the headset's screen");
                    applyRefreshRate();
                }
            } else if (e.state == XR_SESSION_STATE_STOPPING) {
                s_running = false;
                ok(xrEndSession(s_session), "xrEndSession");
            } else if (e.state == XR_SESSION_STATE_EXITING || e.state == XR_SESSION_STATE_LOSS_PENDING) {
                s_running = false;
                s_lost = true;  // (the window goes on; VR again after a restart)
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& e = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(ev);
            logf("xr", "the headset's refresh rate: %g -> %g Hz", e.fromDisplayRefreshRate, e.toDisplayRefreshRate);
            break;
        }
        default: break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---------------------------------------------------------------------------------------
// the screen's resources
// ---------------------------------------------------------------------------------------
void releaseScreen() {
    s_drawTex = {};
    s_drawMem = {};
    s_drawRes.Reset();
    s_images.clear();
    if (s_swapchain) xrDestroySwapchain(s_swapchain);
    s_swapchain = XR_NULL_HANDLE;
    s_width = s_height = 0;
}

bool pickFormat() {
    uint32_t n = 0;
    if (!ok(xrEnumerateSwapchainFormats(s_session, 0, &n, nullptr), "xrEnumerateSwapchainFormats")) return false;
    std::vector<int64_t> formats(n);
    if (!ok(xrEnumerateSwapchainFormats(s_session, n, &n, formats.data()), "xrEnumerateSwapchainFormats")) return false;
    // sRGB first: the runtime then shows the picture's (already encoded) values as they are
    struct Choice {
        DXGI_FORMAT swap, draw;
        wgpu::TextureFormat wgpu;
    };
    static constexpr Choice kChoices[] = {
        {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, wgpu::TextureFormat::RGBA8Unorm},
        {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM, wgpu::TextureFormat::BGRA8Unorm},
        {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, wgpu::TextureFormat::RGBA8Unorm},
        {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, wgpu::TextureFormat::BGRA8Unorm},
    };
    for (const Choice& c : kChoices)
        for (int64_t f : formats)
            if (f == c.swap) {
                s_swapFormat = c.swap;
                s_drawFormat = c.draw;
                s_drawWgpu = c.wgpu;
                logf("xr", "swapchain format %d%s", int(c.swap),
                     c.swap == c.draw ? " (not sRGB: the picture will look washed out)" : "");
                return true;
            }
    logf("xr", "the runtime offers no 8-bit RGBA swapchain format");
    return false;
}

bool createPipeline(const wgpu::Device& device) {
    static constexpr char kWgsl[] = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var smp: sampler;
struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> VO {
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VO;
    o.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
@fragment fn fs(v: VO) -> @location(0) vec4f {
    return vec4f(textureSample(src, smp, v.uv).rgb, 1.0);
}
)";
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kWgsl;
    const wgpu::ShaderModuleDescriptor md{.nextInChain = &wgsl, .label = "xr screen"};
    const wgpu::ShaderModule module = device.CreateShaderModule(&md);
    const wgpu::ColorTargetState target{.format = s_drawWgpu};
    const wgpu::FragmentState fs{.module = module, .entryPoint = "fs", .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor rd{
        .label = "xr screen",
        .vertex = {.module = module, .entryPoint = "vs"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .fragment = &fs,
    };
    s_pipeline = device.CreateRenderPipeline(&rd);

    // the 3D screen: one quad, its corners in the eye's clip space (uniform), a strip
    static constexpr char kScreenWgsl[] = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var smp: sampler;
struct Corners { c: array<vec4f, 4> };
@group(0) @binding(2) var<uniform> u: Corners;
struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> VO {
    var o: VO;
    o.pos = u.c[i];
    o.uv = vec2f(f32(i >> 1u), f32(i & 1u));
    return o;
}
@fragment fn fs(v: VO) -> @location(0) vec4f {
    return vec4f(textureSample(src, smp, v.uv).rgb, 1.0);
}
)";
    wgpu::ShaderSourceWGSL wgsl2{};
    wgsl2.code = kScreenWgsl;
    const wgpu::ShaderModuleDescriptor md2{.nextInChain = &wgsl2, .label = "xr 3D screen"};
    const wgpu::ShaderModule module2 = device.CreateShaderModule(&md2);
    const wgpu::FragmentState fs2{.module = module2, .entryPoint = "fs", .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor rd2{
        .label = "xr 3D screen",
        .vertex = {.module = module2, .entryPoint = "vs"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleStrip},
        .fragment = &fs2,
    };
    s_screenPipeline = device.CreateRenderPipeline(&rd2);
    for (auto& buf : s_cornerBuf) {
        const wgpu::BufferDescriptor bd{.label = "xr 3D screen corners",
                                        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                        .size = 64};
        buf = device.CreateBuffer(&bd);
    }
    const wgpu::SamplerDescriptor sd{.label = "xr screen",
                                     .magFilter = wgpu::FilterMode::Linear,
                                     .minFilter = wgpu::FilterMode::Linear};
    s_sampler = device.CreateSampler(&sd);
    return bool(s_pipeline);
}

// the screen's swapchain and our texture, at the picture's height and the display's aspect
bool createScreen(const wgpu::Device& device, uint32_t w, uint32_t h) {
    releaseScreen();
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = s_swapFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!ok(xrCreateSwapchain(s_session, &ci, &s_swapchain), "xrCreateSwapchain")) return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(s_swapchain, 0, &n, nullptr);
    s_images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (!ok(xrEnumerateSwapchainImages(s_swapchain, n, &n,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(s_images.data())),
            "xrEnumerateSwapchainImages"))
        return false;

    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC rd{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = w,
        .Height = h,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = s_drawFormat,
        .SampleDesc = {.Count = 1},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        // (Dawn imports only resources other queues may use at the same time)
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
    };
    if (FAILED(s_d3d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(&s_drawRes)))) {
        logf("xr", "could not create the screen texture (%ux%u)", w, h);
        return false;
    }
    dawn::native::d3d12::SharedTextureMemoryD3D12ResourceDescriptor imp;
    imp.resource = s_drawRes;
    const wgpu::SharedTextureMemoryDescriptor md{.nextInChain = &imp, .label = "xr screen"};
    s_drawMem = device.ImportSharedTextureMemory(&md);
    if (s_drawMem) s_drawTex = s_drawMem.CreateTexture();
    if (!s_drawTex) {
        logf("xr", "Dawn could not import the screen texture");
        return false;
    }
    s_width = w;
    s_height = h;
    logf("xr", "screen %ux%u, %u swapchain images", w, h, n);
    return true;
}

// ---------------------------------------------------------------------------------------
// per frame (Aurora's render worker)
// ---------------------------------------------------------------------------------------
float displayAspect(uint32_t srcW, uint32_t srcH) {
    const float a = settings::aspectValue(settings::get().aspect);
    if (a > 0.f) return a;
    return srcH ? float(srcW) / float(srcH) : 4.f / 3.f;
}

// draws the pictures into our texture (Dawn) side by side, each into its share of the width,
// then waits on Dawn's fences on the queue
bool drawPicture(const wgpu::Device& device, const wgpu::Queue& queue, const wgpu::Texture* sources, int count) {
    wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
    begin.initialized = false;  // (cleared below)
    if (!s_drawMem.BeginAccess(s_drawTex, &begin)) return false;
    const wgpu::CommandEncoder enc = device.CreateCommandEncoder();
    const wgpu::RenderPassColorAttachment att{.view = s_drawTex.CreateView(),
                                              .loadOp = wgpu::LoadOp::Clear,
                                              .storeOp = wgpu::StoreOp::Store,
                                              .clearValue = {0, 0, 0, 1}};
    const wgpu::RenderPassDescriptor pd{.label = "xr screen", .colorAttachmentCount = 1, .colorAttachments = &att};
    const wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&pd);
    pass.SetPipeline(s_pipeline);
    const float slot = float(s_width) / float(count);
    for (int i = 0; i < count; i++) {
        const wgpu::TextureView srcView = sources[i].CreateView();
        const wgpu::BindGroupEntry entries[2] = {{.binding = 0, .textureView = srcView},
                                                 {.binding = 1, .sampler = s_sampler}};
        const wgpu::BindGroupDescriptor bd{
            .layout = s_pipeline.GetBindGroupLayout(0), .entryCount = 2, .entries = entries};
        pass.SetBindGroup(0, device.CreateBindGroup(&bd));
        pass.SetViewport(slot * float(i), 0.f, slot, float(s_height), 0.f, 1.f);
        pass.Draw(3);
    }
    pass.End();
    const wgpu::CommandBuffer cb = enc.Finish();
    queue.Submit(1, &cb);
    wgpu::SharedTextureMemoryEndAccessState end{};
    if (!s_drawMem.EndAccess(s_drawTex, &end)) return false;
    // the copy waits for Dawn's draw (on the same queue in practice; the fences make it sure)
    for (size_t i = 0; i < end.fenceCount; i++) {
        wgpu::SharedFenceDXGISharedHandleExportInfo dx{};
        wgpu::SharedFenceExportInfo info{.nextInChain = &dx};
        end.fences[i].ExportInfo(&info);
        if (!dx.handle) continue;
        ComPtr<ID3D12Fence> f;
        if (SUCCEEDED(s_d3d->OpenSharedHandle(static_cast<HANDLE>(const_cast<void*>(dx.handle)), IID_PPV_ARGS(&f))))
            s_queue->Wait(f.Get(), end.signaledValues[i]);
    }
    return true;
}

// The 3D screen: each eye's picture on the virtual screen (a quad in LOCAL space, width
// s_screenWidth at s_screenDistance, aspect `aspect`), seen from that eye (views), into its half
// of our texture. False: not drawn.
bool drawScreen3d(const wgpu::Device& device, const wgpu::Queue& queue, const wgpu::Texture* sources,
                  const XrView* views, float aspect) {
    const float hw = s_screenWidth * 0.5f, hh = s_screenWidth / aspect * 0.5f, d = -s_screenDistance;
    // corners: top-left, bottom-left, top-right, bottom-right (the strip's order)
    const float corners[4][3] = {{-hw, hh, d}, {-hw, -hh, d}, {hw, hh, d}, {hw, -hh, d}};
    for (int e = 0; e < 2; e++) {
        const XrQuaternionf& q = views[e].pose.orientation;
        const XrVector3f& pos = views[e].pose.position;
        // R (eye -> LOCAL) as rows; the eye's coordinates are R^T (p - pos)
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        const float r[3][3] = {
            {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
        };
        const float tl = std::tan(views[e].fov.angleLeft), tr = std::tan(views[e].fov.angleRight);
        const float td = std::tan(views[e].fov.angleDown), tu = std::tan(views[e].fov.angleUp);
        float clip[4][4];
        for (int c = 0; c < 4; c++) {
            const float v[3] = {corners[c][0] - pos.x, corners[c][1] - pos.y, corners[c][2] - pos.z};
            float p[3];
            for (int i = 0; i < 3; i++) p[i] = r[0][i] * v[0] + r[1][i] * v[1] + r[2][i] * v[2];
            clip[c][0] = (2.f * p[0] + (tr + tl) * p[2]) / (tr - tl);
            clip[c][1] = (2.f * p[1] + (tu + td) * p[2]) / (tu - td);
            clip[c][2] = 0.f;
            clip[c][3] = -p[2];
        }
        queue.WriteBuffer(s_cornerBuf[e], 0, clip, sizeof(clip));
    }
    wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
    begin.initialized = false;  // (cleared below)
    if (!s_drawMem.BeginAccess(s_drawTex, &begin)) return false;
    const wgpu::CommandEncoder enc = device.CreateCommandEncoder();
    const wgpu::RenderPassColorAttachment att{.view = s_drawTex.CreateView(),
                                              .loadOp = wgpu::LoadOp::Clear,
                                              .storeOp = wgpu::StoreOp::Store,
                                              .clearValue = {0, 0, 0, 1}};
    const wgpu::RenderPassDescriptor pd{.label = "xr 3D screen", .colorAttachmentCount = 1, .colorAttachments = &att};
    const wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&pd);
    pass.SetPipeline(s_screenPipeline);
    for (int e = 0; e < 2; e++) {
        const wgpu::TextureView srcView = sources[e].CreateView();
        const wgpu::BindGroupEntry entries[3] = {{.binding = 0, .textureView = srcView},
                                                 {.binding = 1, .sampler = s_sampler},
                                                 {.binding = 2, .buffer = s_cornerBuf[e], .size = 64}};
        const wgpu::BindGroupDescriptor bd{
            .layout = s_screenPipeline.GetBindGroupLayout(0), .entryCount = 3, .entries = entries};
        pass.SetBindGroup(0, device.CreateBindGroup(&bd));
        pass.SetViewport(float(s_eyeSize * e), 0.f, float(s_eyeSize), float(s_eyeSize), 0.f, 1.f);
        pass.SetScissorRect(s_eyeSize * e, 0, s_eyeSize, s_eyeSize);
        pass.Draw(4);
    }
    pass.End();
    const wgpu::CommandBuffer cb = enc.Finish();
    queue.Submit(1, &cb);
    wgpu::SharedTextureMemoryEndAccessState end{};
    if (!s_drawMem.EndAccess(s_drawTex, &end)) return false;
    for (size_t i = 0; i < end.fenceCount; i++) {
        wgpu::SharedFenceDXGISharedHandleExportInfo dx{};
        wgpu::SharedFenceExportInfo info{.nextInChain = &dx};
        end.fences[i].ExportInfo(&info);
        if (!dx.handle) continue;
        ComPtr<ID3D12Fence> f;
        if (SUCCEEDED(s_d3d->OpenSharedHandle(static_cast<HANDLE>(const_cast<void*>(dx.handle)), IID_PPV_ARGS(&f))))
            s_queue->Wait(f.Get(), end.signaledValues[i]);
    }
    return true;
}

// our texture into the swapchain image (in RENDER_TARGET state while the app holds it)
void copyToImage(ID3D12Resource* image) {
    const int slot = int(s_frame % kSlots);
    if (s_fence->GetCompletedValue() < s_slotValue[slot]) {
        s_fence->SetEventOnCompletion(s_slotValue[slot], s_fenceEvent);
        WaitForSingleObject(s_fenceEvent, 1000);
    }
    s_alloc[slot]->Reset();
    s_list->Reset(s_alloc[slot].Get(), nullptr);
    D3D12_RESOURCE_BARRIER b{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition = {.pResource = image,
                    .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    .StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
                    .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST};
    s_list->ResourceBarrier(1, &b);
    s_list->CopyResource(image, s_drawRes.Get());  // (ours: simultaneous access, no barrier)
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    s_list->ResourceBarrier(1, &b);
    s_list->Close();
    ID3D12CommandList* lists[] = {s_list.Get()};
    s_queue->ExecuteCommandLists(1, lists);
    s_queue->Signal(s_fence.Get(), ++s_fenceValue);
    s_slotValue[slot] = s_fenceValue;
}

// ---------------------------------------------------------------------------------------
// controllers
// ---------------------------------------------------------------------------------------
XrPath path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(s_instance, s, &p);
    return p;
}

bool createActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "gameplay");
    strcpy(si.localizedActionSetName, "Gameplay");
    if (!ok(xrCreateActionSet(s_instance, &si, &s_actionSet), "xrCreateActionSet")) return false;
    struct Def {
        const char* name;
        const char* label;
        XrActionType type;
        const char* binding;
    };
    static constexpr Def kDefs[kActs] = {
        {"trigger_left", "L (throttle)", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/trigger/value"},
        {"trigger_right", "R (brake)", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/trigger/value"},
        {"grip_left", "LB", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/squeeze/value"},
        {"grip_right", "RB (Z)", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/squeeze/value"},
        {"stick_left", "Control stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/left/input/thumbstick"},
        {"stick_right", "C-stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/right/input/thumbstick"},
        {"button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/a/click"},
        {"button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/b/click"},
        {"button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/x/click"},
        {"button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/y/click"},
        {"button_start", "Start", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/menu/click"},
        {"click_left", "Back (menu)", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/thumbstick/click"},
        {"click_right", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/thumbstick/click"},
    };
    std::array<XrActionSuggestedBinding, kActs + 2> bindings{};
    for (int i = 0; i < kActs; i++) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        strcpy(ai.actionName, kDefs[i].name);
        strcpy(ai.localizedActionName, kDefs[i].label);
        ai.actionType = kDefs[i].type;
        if (!ok(xrCreateAction(s_actionSet, &ai, &s_act[i]), "xrCreateAction")) return false;
        bindings[i] = {s_act[i], path(kDefs[i].binding)};
    }
    {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        strcpy(ai.actionName, "haptic");
        strcpy(ai.localizedActionName, "Vibration (firing)");
        ai.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        if (!ok(xrCreateAction(s_actionSet, &ai, &s_haptic), "xrCreateAction (haptic)")) return false;
        bindings[kActs] = {s_haptic, path("/user/hand/left/output/haptic")};
        bindings[kActs + 1] = {s_haptic, path("/user/hand/right/output/haptic")};
    }
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = u32(bindings.size());
    sb.suggestedBindings = bindings.data();
    if (!ok(xrSuggestInteractionProfileBindings(s_instance, &sb), "xrSuggestInteractionProfileBindings")) return false;
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &s_actionSet;
    return ok(xrAttachSessionActionSets(s_session, &at), "xrAttachSessionActionSets");
}

void createPad() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Red Two VR controllers";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    s_joy = id ? SDL_OpenJoystick(id) : nullptr;
    logf("xr", "controllers: virtual gamepad %s", s_joy ? "attached" : SDL_GetError());
}

float actFloat(Act a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = s_act[a];
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s_session, &gi, &st)) && st.isActive ? st.currentState : 0.f;
}
XrVector2f actStick(Act a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = s_act[a];
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(s_session, &gi, &st)) && st.isActive) return st.currentState;
    return XrVector2f{0.f, 0.f};
}
bool actBool(Act a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = s_act[a];
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s_session, &gi, &st)) && st.isActive && st.currentState;
}

// the controllers into the virtual gamepad (neutral while the headset does not have the focus)
void updatePad() {
    if (!s_joy) return;
    const bool focused = s_running && s_state == XR_SESSION_STATE_FOCUSED;
    if (!focused && !s_padLive) return;
    bool synced = false;
    if (focused) {
        const XrActiveActionSet active{s_actionSet, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        synced = XR_SUCCEEDED(xrSyncActions(s_session, &si));
    }
    auto axis = [](float v) { return Sint16(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f)); };
    XrVector2f l = synced ? actStick(StickL) : XrVector2f{0.f, 0.f};
    // The d-pad (the squadron's orders in flight; the Touch controllers have none): the left grip
    // held and the left thumbstick pushed. As with the right grip, the grip stays LB for the game
    // until the stick moves; from then until the grip is let go the game sees neither LB nor the
    // stick, only the d-pad direction (the stronger axis).
    static bool s_dpadMode = false;
    const bool gripL = synced && actFloat(GripL) > 0.5f;
    if (!gripL) s_dpadMode = false;
    else if (std::max(std::abs(l.x), std::abs(l.y)) > 0.5f) s_dpadMode = true;
    bool dUp = false, dDown = false, dLeft = false, dRight = false;
    if (s_dpadMode) {
        if (std::max(std::abs(l.x), std::abs(l.y)) > 0.5f) {
            if (std::abs(l.y) >= std::abs(l.x)) (l.y > 0.f ? dUp : dDown) = true;
            else (l.x > 0.f ? dRight : dLeft) = true;
        }
        l = XrVector2f{0.f, 0.f};
    }
    XrVector2f r = synced ? actStick(StickR) : XrVector2f{0.f, 0.f};
    // Stereo tuning: the right grip held, the right thumbstick's right/left sets the depth
    // (stronger/weaker), and in flight its up/down moves the eyes toward the craft / back (the
    // game's own nearest view brought closer). The grip stays RB (Z) for the game until the stick
    // moves; from then until the grip is let go the game sees neither RB nor the stick.
    auto dead = [](float v) { return std::abs(v) < 0.2f ? 0.f : v; };
    const bool grip = synced && actFloat(GripR) > 0.5f;
    {
        static bool xWas = false, yWas = false;
        const bool xb = synced && actBool(BtnX), yb = synced && actBool(BtnY);
        if (!s_flight) {
            s_cockpit = s_targeting = false;
        } else {
            if (xb && !xWas) s_cockpit = !s_cockpit;
            if (settings::get().targetingToggle) {
                if (yb && !yWas) s_targeting = !s_targeting;
            } else {
                s_targeting = yb;
            }
        }
        xWas = xb;
        yWas = yb;
    }
    // (without the grip up/down stays the game's: its camera change, cockpit / near / far)
    const bool tuning = s_stereo && grip && (s_tuning || dead(r.x) != 0.f || (s_flight && !cockpitView() && dead(r.y) != 0.f));
    if (tuning) {
        const float dt = 1.f / 60.f;  // (once per presented frame)
        if (s_flight) {
            s_worldScale = std::clamp(s_worldScale.load() * std::exp(dead(r.x) * 1.2f * dt), 0.05f, 1000.f);
            // metres (times the world scale: game units) a second at full tilt
            if (!cockpitView())
                s_cameraForward = std::clamp(s_cameraForward.load() + dead(r.y) * 6.f * dt, -20.f, 200.f);
        } else {  // (the 3D screen: its depth)
            s_screenDepth = std::clamp(s_screenDepth.load() * std::exp(dead(r.x) * 1.2f * dt), 0.5f, 5000.f);
        }
        r.x = r.y = 0.f;
    } else if (s_tuning) {
        s_tuneSave = true;
        logf("xr", "tuned: world scale %g, eyes %g m ahead, screen depth %g", s_worldScale.load(), s_cameraForward.load(),
             s_screenDepth.load());
    }
    s_tuning = tuning;
    SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_LEFTX, axis(l.x));
    SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_LEFTY, axis(-l.y));  // (SDL: down is positive)
    SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_RIGHTX, axis(r.x));
    SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_RIGHTY, axis(-r.y));
    SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis(synced ? actFloat(TrigL) : 0.f));
    SDL_SetJoystickVirtualAxis(s_joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis(synced ? actFloat(TrigR) : 0.f));
    auto button = [synced](SDL_GamepadButton b, bool down) { SDL_SetJoystickVirtualButton(s_joy, b, synced && down); };
    button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, gripL && !s_dpadMode);
    button(SDL_GAMEPAD_BUTTON_DPAD_UP, dUp);
    button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, dDown);
    button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, dLeft);
    button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, dRight);
    button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, grip && !tuning);
    button(SDL_GAMEPAD_BUTTON_SOUTH, synced && actBool(BtnA));
    button(SDL_GAMEPAD_BUTTON_EAST, synced && actBool(BtnB));
    button(SDL_GAMEPAD_BUTTON_WEST, synced && actBool(BtnX));
    button(SDL_GAMEPAD_BUTTON_NORTH, synced && actBool(BtnY));
    button(SDL_GAMEPAD_BUTTON_START, synced && actBool(BtnMenu));
    button(SDL_GAMEPAD_BUTTON_BACK, synced && actBool(ClickL));
    button(SDL_GAMEPAD_BUTTON_RIGHT_STICK, synced && actBool(ClickR));
    s_padLive = synced;
    // a pending vibration (haptic, from the game's thread)
    float amp, secs;
    {
        std::lock_guard lock(s_hapticMutex);
        amp = s_hapticAmp;
        secs = s_hapticSecs;
        s_hapticAmp = s_hapticSecs = 0.f;
    }
    // (a weaker pulse does not cut a stronger one short: a laser shot during a death's rumble)
    const auto now = std::chrono::steady_clock::now();
    if (now >= s_hapticUntil) s_hapticPlaying = 0.f;
    if (synced && s_haptic && amp > 0.f && amp >= s_hapticPlaying) {
        s_hapticPlaying = amp;
        s_hapticUntil = now + std::chrono::microseconds(std::lround(double(secs) * 1e6));
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};  // (no subaction path: both hands)
        hi.action = s_haptic;
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = amp;
        v.duration = XrDuration(double(secs) * 1e9);
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        xrApplyHapticFeedback(s_session, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
    }
}

}  // namespace

void haptic(float amplitude, float seconds) {
    std::lock_guard lock(s_hapticMutex);
    if (amplitude < s_hapticAmp) return;
    s_hapticAmp = std::clamp(amplitude, 0.f, 1.f);
    s_hapticSecs = seconds;
}

namespace {

// (also while a cinematic is skipped: nothing is drawn but the window's "SKIPPING", ImGui too)
bool menusUp() { return gfx::launcherShowing() || gfx::menuOpen() || cinematicSkip(); }

void onPresent(const wgpu::Device& device, const wgpu::Queue& queue, const wgpu::Texture& frame,
               const wgpu::Texture& right, const wgpu::Texture& window, uint32_t frameNumber, void*) {
    pollEvents();
    updatePad();
    if (frame) {  // (the game's frame: each eye's size in stereo)
        static uint32_t lw = 0, lh = 0;
        if (frame.GetWidth() != lw || frame.GetHeight() != lh) {
            lw = frame.GetWidth();
            lh = frame.GetHeight();
            logf("xr", "game frame %ux%u%s", lw, lh, right ? " per eye" : "");
        }
    }
    if (!s_running || s_lost) return;
    // the launcher and the F1 menu are ImGui, drawn over the window only: the window's picture
    // then (its own shape, bars included); the game's frame otherwise (the internal resolution)
    const bool menus = window && (menusUp() || !frame);
    const wgpu::Texture& source = menus ? window : frame;
    if (!source) return;
    // stereo: the views this frame was drawn with
    FrameViews drawn{};
    if (s_stereo && !menus && right) {
        std::lock_guard lock{s_viewsMutex};
        const FrameViews& f = s_frameViews[frameNumber % s_frameViews.size()];
        if (f.valid && f.frame == frameNumber) drawn = f;
    }
    const bool stereoFrame = drawn.valid;
    const bool screen3d = stereoFrame && drawn.screen;  // (both eyes on the virtual screen)
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!ok(xrWaitFrame(s_session, nullptr, &fs), "xrWaitFrame")) return;
    s_displayTime = fs.predictedDisplayTime;
    s_displayPeriod = fs.predictedDisplayPeriod;
    if (!ok(xrBeginFrame(s_session, nullptr), "xrBeginFrame")) return;
    s_frame++;

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projViews[2]{};
    const XrCompositionLayerBaseHeader* layers[2] = {};
    uint32_t layerCount = 0;
    if (fs.shouldRender) {
        const uint32_t srcW = source.GetWidth(), srcH = source.GetHeight();
        const float aspect = menus ? float(srcW) / float(srcH) : displayAspect(srcW, srcH);
        // stereo: both eyes side by side, each at the size it was drawn at (on the 3D screen at
        // the display's shape, like the flat one)
        const uint32_t eyeW = uint32_t(std::lround(float(srcH) * aspect)) & ~1u;
        if (screen3d && !s_eyeSize) {
            // the projection layer's eyes at the runtime's recommended size
            XrViewConfigurationView vc[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
            uint32_t vn = 0;
            if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(s_instance, s_system,
                                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vn, vc)) &&
                vn == 2)
                s_eyeSize = std::max(vc[0].recommendedImageRectWidth, vc[0].recommendedImageRectHeight);
            s_eyeSize = std::clamp(s_eyeSize ? s_eyeSize : 1600u, 512u, 4096u);
        }
        const uint32_t w = screen3d ? s_eyeSize * 2 : stereoFrame ? srcW * 2 : eyeW;
        const uint32_t h = screen3d ? s_eyeSize : srcH;
        bool ready = w && h && (w == s_width && h == s_height || createScreen(device, w, h));
        uint32_t index = 0;
        if (ready) {
            const XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            ready = ok(xrAcquireSwapchainImage(s_swapchain, &ai, &index), "xrAcquireSwapchainImage");
        }
        if (ready) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            bool waited = ok(xrWaitSwapchainImage(s_swapchain, &wi), "xrWaitSwapchainImage");
            const wgpu::Texture eyes[2] = {source, right};
            // the 3D screen: seen from where the eyes will be when it shows
            XrView screenViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            bool screenLocated = false;
            if (screen3d) {
                XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
                li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                li.displayTime = fs.predictedDisplayTime;
                li.space = s_space;
                XrViewState vs{XR_TYPE_VIEW_STATE};
                uint32_t vn = 0;
                screenLocated = XR_SUCCEEDED(xrLocateViews(s_session, &li, &vs, 2, &vn, screenViews)) && vn == 2 &&
                                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
                if (screenLocated && !(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
                    for (auto& v : screenViews) v.pose.position = {0.f, 0.f, 0.f};
            }
            const bool drewPicture = waited && (screen3d ? screenLocated && drawScreen3d(device, queue, eyes, screenViews, aspect)
                                              : drawPicture(device, queue, eyes, stereoFrame ? 2 : 1));
            if (drewPicture) copyToImage(s_images[index].texture);
            const XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (waited) ok(xrReleaseSwapchainImage(s_swapchain, &ri), "xrReleaseSwapchainImage");
            if (waited && screen3d && screenLocated) {
                // the screen drawn into each eye's view: a projection layer
                for (int e = 0; e < 2; e++) {
                    projViews[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    projViews[e].pose = screenViews[e].pose;
                    projViews[e].fov = screenViews[e].fov;
                    projViews[e].subImage.swapchain = s_swapchain;
                    projViews[e].subImage.imageRect = {{int32_t(s_eyeSize) * e, 0},
                                                       {int32_t(s_eyeSize), int32_t(s_eyeSize)}};
                    projViews[e].subImage.imageArrayIndex = 0;
                }
                proj.space = s_space;
                proj.viewCount = 2;
                proj.views = projViews;
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
            } else if (waited && stereoFrame && !screen3d) {
                for (int e = 0; e < 2; e++) {
                    projViews[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    projViews[e].pose = drawn.views[e].pose;
                    projViews[e].fov = drawn.views[e].fov;
                    projViews[e].subImage.swapchain = s_swapchain;
                    projViews[e].subImage.imageRect = {{int32_t(srcW) * e, 0}, {int32_t(srcW), int32_t(h)}};
                    projViews[e].subImage.imageArrayIndex = 0;
                }
                proj.space = s_space;
                proj.viewCount = 2;
                proj.views = projViews;
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
            } else if (waited && !screen3d) {
                quad.space = s_space;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = s_swapchain;
                quad.subImage.imageRect = {{0, 0}, {int32_t(w), int32_t(h)}};
                quad.subImage.imageArrayIndex = 0;
                quad.pose = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, -s_screenDistance}};
                quad.size = {s_screenWidth, s_screenWidth * float(h) / float(w)};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            }
        }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    ok(xrEndFrame(s_session, &ei), "xrEndFrame");
}

}  // namespace

// ---------------------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------------------
bool preinit(int argc, char** argv) {
    if (const char* f = getenv("R2_XR_FAKE"); f && atoi(f)) {
        s_fake = s_stereo = true;
        aurora::gfx::set_stereo(true);
        aurora::gfx::set_stereo_hud_distance(settings::get().xrHudDistance);
        s_worldScale = settings::get().xrWorldScale;
        s_screenDepth = settings::get().xrScreenDepth;
        logf("xr", "R2_XR_FAKE: stereo with made-up eyes, no headset");
        return false;
    }
    if (!asked(argc, argv)) return false;
    s_screenWidth = settings::get().xrScreenWidth;
    if (const char* s = getenv("R2_XR_SCREEN")) {
        float w = 0.f, d = 0.f;
        if (sscanf(s, "%f,%f", &w, &d) == 2 && w > 0.1f && d > 0.1f) {
            s_screenWidth = w;
            s_screenDistance = d;
        }
    }
    const char* exts[2] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    uint32_t extCount = 1;
    {
        // the refresh rate extension, if the runtime has it (setting xr_refresh_rate)
        uint32_t n = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        if (n && XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data())))
            for (const auto& p : props)
                if (!strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
                    exts[extCount++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
        if (extCount == 1) logf("xr", "the runtime cannot set the headset's refresh rate (no %s)", XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "Red Two");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "Aurora");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = extCount;
    ci.enabledExtensionNames = exts;
    if (!ok(xrCreateInstance(&ci, &s_instance), "xrCreateInstance (is an OpenXR runtime installed and active?)"))
        return false;
    if (extCount > 1) {
        xrGetInstanceProcAddr(s_instance, "xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&s_enumRates));
        xrGetInstanceProcAddr(s_instance, "xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&s_requestRate));
        xrGetInstanceProcAddr(s_instance, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&s_getRate));
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(s_instance, &ip)))
        logf("xr", "OpenXR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
             XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(xrGetSystem(s_instance, &si, &s_system), "xrGetSystem (is the headset connected?)")) {
        xrDestroyInstance(s_instance);
        s_instance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(s_instance, s_system, &sp))) logf("xr", "headset: %s", sp.systemName);
    PFN_xrGetD3D12GraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(s_instance, "xrGetD3D12GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (!getReqs || !ok(getReqs(s_instance, s_system, &req), "xrGetD3D12GraphicsRequirementsKHR")) {
        xrDestroyInstance(s_instance);
        s_instance = XR_NULL_HANDLE;
        return false;
    }
    aurora::gfx::set_adapter_luid(req.adapterLuid.LowPart, req.adapterLuid.HighPart);
    const char* mode = getenv("R2_XR_MODE");  // (tests: "screen" or "stereo" over the setting)
    s_stereo = mode ? strcmp(mode, "screen") != 0 : settings::get().xrStereo;
    s_worldScale = settings::get().xrWorldScale;
    s_cameraForward = settings::get().xrCameraForward;
    s_screenDepth = settings::get().xrScreenDepth;
    if (const char* sc = getenv("R2_XR_SCALE")) s_worldScale = std::max(0.001f, float(atof(sc)));
    if (s_stereo) aurora::gfx::set_stereo(true);
    aurora::gfx::set_stereo_hud_distance(settings::get().xrHudDistance);
    logf("xr", "mode: %s", s_stereo ? "stereo 3D" : "flat screen");
    if (s_stereo)
        logf("xr", "world scale %g game units per metre, eyes %g m ahead of the camera", s_worldScale.load(),
             s_cameraForward.load());
    return true;
}

void start() {
    if (!s_instance) return;
    auto fail = [](const char* why) {
        logf("xr", "no VR this run: %s", why);
        if (s_session) xrDestroySession(s_session);
        s_session = XR_NULL_HANDLE;
        xrDestroyInstance(s_instance);
        s_instance = XR_NULL_HANDLE;
    };
    if (aurora_get_backend() != BACKEND_D3D12) return fail("Aurora is not on D3D12");
    const wgpu::Device device = aurora::gfx::device();
    if (!device.HasFeature(wgpu::FeatureName::SharedTextureMemoryD3D12Resource))
        return fail("the device cannot import D3D12 textures (SharedTextureMemoryD3D12Resource)");
    s_d3d = dawn::native::d3d12::GetD3D12Device(device.Get());
    s_queue = dawn::native::d3d12::GetD3D12CommandQueue(device.Get());
    if (!s_d3d || !s_queue) return fail("no D3D12 device from Dawn");

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = s_d3d.Get();
    binding.queue = s_queue.Get();
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = s_system;
    if (!ok(xrCreateSession(s_instance, &sci, &s_session), "xrCreateSession")) return fail("no session");
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
    if (!ok(xrCreateReferenceSpace(s_session, &rs, &s_space), "xrCreateReferenceSpace")) return fail("no space");
    if (!pickFormat()) return fail("no swapchain format");
    if (!createPipeline(device)) return fail("no pipeline");
    if (createActions()) createPad();  // (without them the screen still works)

    for (auto& a : s_alloc)
        if (FAILED(s_d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
            return fail("no command allocator");
    if (FAILED(s_d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alloc[0].Get(), nullptr,
                                        IID_PPV_ARGS(&s_list))))
        return fail("no command list");
    s_list->Close();
    if (FAILED(s_d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fence)))) return fail("no fence");
    s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    logf("xr", "session created; screen %.2f m wide, %.2f m away", s_screenWidth, s_screenDistance);
    aurora::gfx::set_present_hook(onPresent, nullptr);
}

bool active() { return s_running || s_fake; }
bool enabled() { return s_session != XR_NULL_HANDLE || s_fake; }
int recommendedEyeHeight() {
    if (s_system == XR_NULL_SYSTEM_ID) return 0;
    XrViewConfigurationView vc[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateViewConfigurationViews(s_instance, s_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n,
                                                    vc)) || n != 2)
        return 0;
    return int(vc[0].recommendedImageRectHeight);
}

void resetCameraZoom() {
    s_cameraForward = 0.f;
    settings::get().xrCameraForward = 0.f;
    logf("xr", "camera zoom reset: the eyes at the game's camera");
}

void applySettings() {
    const settings::Settings& st = settings::get();
    aurora::gfx::set_stereo_hud_distance(st.xrHudDistance);
    s_screenDepth = st.xrScreenDepth;
    if (!getenv("R2_XR_SCREEN")) s_screenWidth = st.xrScreenWidth;
    s_rateDirty = true;  // (the render worker asks: pollEvents)
}

bool stereo() { return s_stereo; }

// the eye's view: eye space from the game's view space (the game's camera at the LOCAL space's
// origin, in game units: the eye's position scaled), 3x4 rows = the inverse of the eye's pose
// forward: game units the eyes sit ahead of the game's camera (along its view, -z)
static void eyeView(const XrPosef& pose, float scale, float forward, float* v) {
    const float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
    // R (the eye's orientation) as rows; the view takes its transpose
    const float r[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    const float p[3] = {pose.position.x * scale, pose.position.y * scale, pose.position.z * scale};
    for (int i = 0; i < 3; i++) {
        v[i * 4 + 0] = r[0][i];
        v[i * 4 + 1] = r[1][i];
        v[i * 4 + 2] = r[2][i];
        v[i * 4 + 3] = -(r[0][i] * p[0] + r[1][i] * p[1] + r[2][i] * (p[2] - forward));
    }
}

void setFlight(bool flying) {
    s_flight = flying;
    if (!flying) s_camValid = false;
}

// the eyes' predicted views (the time frameViews and cameraRotation share)
bool locateEyes(XrView views[2], XrViewState& vs) {
    if (s_fake) {
        static const float yaw = [] {
            const char* e = getenv("R2_XR_FAKE_YAW");
            return e ? float(atof(e)) * 3.14159265f / 180.f : 0.f;
        }();
        static const float pitch = [] {  // (R2_XR_FAKE_PITCH=<degrees>: up; negative, down)
            const char* e = getenv("R2_XR_FAKE_PITCH");
            return e ? float(atof(e)) * 3.14159265f / 180.f : 0.f;
        }();
        const float sy = std::sin(yaw * 0.5f), cy = std::cos(yaw * 0.5f);
        const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
        for (int e = 0; e < 2; e++) {
            views[e] = {XR_TYPE_VIEW};
            views[e].pose.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};  // (yaw, then pitch)
            views[e].pose.position = {e == 0 ? -0.032f : 0.032f, 0.f, 0.f};
            views[e].fov = {-0.7854f, 0.7854f, 0.6435f, -0.6435f};  // (left right up down: 90 x 73.7 degrees, 4:3)
        }
        vs = {XR_TYPE_VIEW_STATE};
        return true;
    }
    const XrTime last = s_displayTime.load();
    if (!last) return false;
    // shown about two headset frames after the last one the render worker waited for
    const XrTime when = last + 2 * s_displayPeriod.load();
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = when;
    li.space = s_space;
    vs = {XR_TYPE_VIEW_STATE};
    views[0] = {XR_TYPE_VIEW};
    views[1] = {XR_TYPE_VIEW};
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(s_session, &li, &vs, 2, &n, views)) || n != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return false;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
        for (int e = 0; e < 2; e++) views[e].pose.position = {0.f, 0.f, 0.f};
    return true;
}

XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrQuaternionf qconj(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }
XrVector3f qrot(const XrQuaternionf& q, const XrVector3f& v) {
    const XrQuaternionf p = qmul(qmul(q, {v.x, v.y, v.z, 0.f}), qconj(q));
    return {p.x, p.y, p.z};
}

void setGameView(float fovY, float aspect) {
    if (!s_stereo) return;
    const float th = std::tan(fovY * 0.5f * 3.14159265f / 180.f);
    aurora::gfx::set_stereo_frame_tan(th * aspect, th);
    static float lastFov = 0.f, lastAspect = 0.f;
    if (std::abs(fovY - lastFov) > 0.5f || std::abs(aspect - lastAspect) > 0.01f) {
        lastFov = fovY;
        lastAspect = aspect;
        logf("xr", "the game's view for the HUD: fov %.1f, aspect %.3f", fovY, aspect);
    }
}

bool cameraRotation(float r[9]) {
    if (!s_stereo || !s_running || s_lost || menusUp() || !s_flight) return false;
    XrView views[2];
    XrViewState vs;
    if (!locateEyes(views, vs)) return false;
    // the head: between the eyes, looking where they look
    XrQuaternionf q = views[0].pose.orientation;
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float m[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                        2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                        2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    std::copy(m, m + 9, r);
    s_camQ = q;
    s_camValid = true;
    return true;
}

bool frameViews(float out[2][16]) {
    if (s_tuneSave.exchange(false)) {
        settings::get().xrWorldScale = s_worldScale;
        settings::get().xrCameraForward = s_cameraForward;
        settings::get().xrScreenDepth = s_screenDepth;
        settings::save();
    }
    if (!s_stereo || !(s_running || s_fake) || s_lost || menusUp()) return false;
    const bool screen = !s_flight;
    const float scale = s_worldScale;
    XrViewState vs;
    XrView views[2];
    if (!locateEyes(views, vs)) return false;
    // Not flying (the hangar, cinematics, the menus the game draws): the virtual screen, in 3D.
    // Each eye sees the game's own view from its place beside the camera (the headset's eye
    // distance, at the world scale); what lies at the screen's distance (2.5 m) shows at the
    // screen, nearer things before it and farther ones behind.
    if (screen) {
        const XrVector3f &a = views[0].pose.position, &b = views[1].pose.position;
        float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
        if (ipd < 0.04f || ipd > 0.09f) ipd = 0.063f;
        for (int e = 0; e < 2; e++) {
            std::fill(out[e], out[e] + 16, 0.f);
            const float depth = s_screenDepth.load();  // (not the flight's world scale)
            out[e][2] = s_screenDistance * depth;
            out[e][4] = out[e][9] = out[e][14] = 1.f;  // (rows of the 3x4 view: x, y, z)
            out[e][4 + 3] = -(e == 0 ? -0.5f : 0.5f) * ipd * depth;
        }
    }
    // the eyes ahead of the game's camera (game units): none in the cockpit
    const float forward = cockpitView() ? 0.f : scale * s_cameraForward.load();
    for (int e = 0; e < 2 && !screen; e++) {
        const XrFovf& f = views[e].fov;
        out[e][0] = std::tan(f.angleLeft);
        out[e][1] = std::tan(f.angleRight);
        out[e][2] = std::tan(f.angleDown);
        out[e][3] = std::tan(f.angleUp);
        // the game's camera turned with the head (cameraRotation): the eye relative to it
        XrPosef pose = views[e].pose;
        if (s_camValid) {
            const XrQuaternionf inv = qconj(s_camQ);
            pose.orientation = qmul(inv, pose.orientation);
            pose.position = qrot(inv, pose.position);
        }
        eyeView(pose, scale, forward, &out[e][4]);
    }
    aurora::gfx::set_stereo_hud_offset(forward);  // (the HUD keeps its place)
    const uint32_t frame = aurora::gfx::current_frame();
    std::lock_guard lock{s_viewsMutex};
    FrameViews& f = s_frameViews[frame % s_frameViews.size()];
    f.frame = frame;
    f.valid = true;
    f.screen = screen;
    f.views[0] = views[0];
    f.views[1] = views[1];
    return true;
}

}  // namespace rt::xr

#endif  // R2_OPENXR
