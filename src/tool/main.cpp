#include "core/constants.h"
#include "core/finger_control.h"
#include "core/frame_state.h"
#include "core/input_state.h"
#include "core/manipulation.h"
#include "core/mmd_retarget.h"
#include "core/nya_format.h"
#include "core/protocol.h"
#include "core/solved_motion.h"
#include "core/tpose.h"
#include "core/udp_log.h"
#include "tool/driver_control.h"
#include "tool/localization.h"
#include "tool/mmd_dance.h"
#include "tool/resource.h"

// Define before any Windows header so the windows.h min/max macros do not
// shadow std::min/std::max/std::clamp. Guarded so it does not clash with the
// same definitions the build passes on the command line.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <tchar.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "ws2_32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using namespace anyadance;
using namespace anyadance::tool;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_deviceContext = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
ID3D11ShaderResourceView* g_bodyBackgroundTexture = nullptr;
int g_bodyBackgroundWidth = 0;
int g_bodyBackgroundHeight = 0;
ID3D11ShaderResourceView* g_footerBannerTexture = nullptr;
int g_footerBannerWidth = 0;
int g_footerBannerHeight = 0;

ImFont* g_uiFont = nullptr;
ImFont* g_monoFont = nullptr;
ImVector<ImWchar> g_uiGlyphRanges;
constexpr int kDefaultWindowWidth = 980;
constexpr int kDefaultWindowHeight = 780;
constexpr int kMinWindowWidth = 980;
constexpr int kMinWindowHeight = 780;
constexpr int kMiniWindowWidth = 360;
constexpr int kMiniWindowHeight = 480;
constexpr int kMiniMinWindowWidth = 280;
constexpr int kMiniMinWindowHeight = 340;
constexpr float kFingerBendStep = 0.1f;
constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\AnyaDance.SingleInstance";

struct ScopedHandle {
    explicit ScopedHandle(HANDLE value) : handle(value) {}
    ~ScopedHandle() {
        if (handle) {
            CloseHandle(handle);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE handle = nullptr;
};

enum class UiMode {
    Full,
    Mini,
};

// Number keys held while scrolling over the body panel target a single finger.
// 1-5 walk the left hand from pinky to thumb; 6-0 walk the right hand from thumb
// to pinky, so 5/6 are the two thumbs and 1/0 the two pinkies (a mirror around
// the middle). VK_0..VK_9 equal the ASCII digits '0'..'9'. The `name` is the
// English label used in the UDP log, which is always English.
struct FingerKey {
    int vk;
    int hand;  // 0 = left, 1 = right
    anyadance::Finger finger;
    const char* name;
};
constexpr FingerKey kFingerKeys[] = {
    {'1', 0, anyadance::Finger::Pinky,  "Left pinky"},
    {'2', 0, anyadance::Finger::Ring,   "Left ring"},
    {'3', 0, anyadance::Finger::Middle, "Left middle"},
    {'4', 0, anyadance::Finger::Index,  "Left index"},
    {'5', 0, anyadance::Finger::Thumb,  "Left thumb"},
    {'6', 1, anyadance::Finger::Thumb,  "Right thumb"},
    {'7', 1, anyadance::Finger::Index,  "Right index"},
    {'8', 1, anyadance::Finger::Middle, "Right middle"},
    {'9', 1, anyadance::Finger::Ring,   "Right ring"},
    {'0', 1, anyadance::Finger::Pinky,  "Right pinky"},
};

// Mouse pixels of drag from the press point that map to full right-stick
// deflection (1.0) when dragging the empty area of the body panel.
constexpr float kJoystickDragRangePixels = 120.0f;
constexpr wchar_t kWindowClassName[] = L"AnyaDance";
constexpr wchar_t kWindowTitle[] = L"AnyaDance";

// The UDP log is always recorded in English regardless of the UI language.
const char* En(Text id) {
    return Tr(id, Language::English);
}

// Owns the UDP socket and a background thread that streams the current pose to
// the driver at kStreamRateHz. The UI thread pushes new frames (with an optional
// reason that becomes a log entry); the worker thread does the actual sending.
// m_mutex guards the shared frame/socket; m_logMutex guards the log only.
class UdpStreamer {
public:
    ~UdpStreamer() { Stop(); }

    bool Start(const FrameState& initialFrame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return true;
        }
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            m_log.Add(m_socketErrorReason, m_failedResult, {}, "WSAStartup failed");
            return false;
        }
        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET) {
            m_log.Add(m_socketErrorReason, m_failedResult, {}, "socket() failed");
            WSACleanup();
            return false;
        }
        m_destination.sin_family = AF_INET;
        InetPtonA(AF_INET, kUdpHost, &m_destination.sin_addr);
        m_destination.sin_port = htons(kUdpPort);
        m_frame = initialFrame;
        m_running = true;
        m_thread = std::thread(&UdpStreamer::Loop, this);
        return true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) {
                return;
            }
            NeutralizeControllerInputs(m_frame);
            m_pendingReason = m_releaseReason;
            m_pendingManipulation = false;
            m_running = false;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
        }
        WSACleanup();
    }

    void SetLocalizedResults(std::string sent, std::string failed, std::string socketError, std::string releaseReason) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sentResult = std::move(sent);
        m_failedResult = std::move(failed);
        m_socketErrorReason = std::move(socketError);
        m_releaseReason = std::move(releaseReason);
    }

    void UpdateFrame(const FrameState& frame, std::string reason = {}, bool manipulation = false) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_frame = frame;
            if (!reason.empty()) {
                m_pendingReason = std::move(reason);
                m_pendingManipulation = manipulation;
            }
        }
        m_cv.notify_all();
    }

    std::vector<UdpLogEntry> SnapshotLog() const {
        std::lock_guard<std::mutex> lock(m_logMutex);
        return std::vector<UdpLogEntry>(m_log.Entries().begin(), m_log.Entries().end());
    }

    void ClearLog() {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_log.Clear();
    }

    // Resend a captured payload once over the same UDP socket and log the result.
    // Holding m_mutex keeps Stop() from closing the socket mid-send. Used by the
    // log detail dialog's Resend button so a past datagram can be replayed.
    bool SendRaw(const std::string& payload, const std::string& reason) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running || m_socket == INVALID_SOCKET) {
            std::lock_guard<std::mutex> logLock(m_logMutex);
            m_log.Add(m_socketErrorReason, m_failedResult, payload, "not streaming");
            return false;
        }
        const int error = SendDatagram(payload);
        std::lock_guard<std::mutex> logLock(m_logMutex);
        if (error != 0) {
            m_log.Add(m_socketErrorReason, m_failedResult, payload, "WSA error " + std::to_string(error));
            return false;
        }
        m_log.Add(reason, m_sentResult, payload);
        return true;
    }

private:
    // Send one datagram to the configured destination. Returns 0 on success or
    // the WSA error code on failure; the caller owns logging so the streaming
    // loop and the on-demand resend can format their own log entries.
    int SendDatagram(const std::string& payload) {
        const int result = sendto(
            m_socket,
            payload.data(),
            static_cast<int>(payload.size()),
            0,
            reinterpret_cast<const sockaddr*>(&m_destination),
            sizeof(m_destination));
        return result == SOCKET_ERROR ? WSAGetLastError() : 0;
    }

    void Loop() {
        auto nextSend = std::chrono::steady_clock::now();
        while (true) {
            FrameState frame;
            std::string reason;
            bool manipulation = false;
            bool shouldExit = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_until(lock, nextSend, [this] { return !m_running || !m_pendingReason.empty(); });
                frame = m_frame;
                reason = std::move(m_pendingReason);
                manipulation = m_pendingManipulation;
                m_pendingReason.clear();
                m_pendingManipulation = false;
                shouldExit = !m_running;
            }

            // Always send the latest pose; only write a log entry when a reason
            // is attached so the 60 Hz idle stream does not flood the log.
            const std::string payload = SerializeFrame(frame);
            const int error = SendDatagram(payload);
            if (error != 0) {
                std::lock_guard<std::mutex> logLock(m_logMutex);
                m_log.Add(m_socketErrorReason, m_failedResult, payload, "WSA error " + std::to_string(error));
            } else if (!reason.empty()) {
                std::lock_guard<std::mutex> logLock(m_logMutex);
                if (manipulation) {
                    m_log.AddManipulation(m_sentResult, payload, reason);
                } else {
                    m_log.Add(reason, m_sentResult, payload);
                }
            }

            if (shouldExit) {
                break;
            }
            nextSend = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000 / kStreamRateHz);
        }
    }

    mutable std::mutex m_logMutex;
    UdpLog m_log;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    FrameState m_frame{};
    SOCKET m_socket = INVALID_SOCKET;
    sockaddr_in m_destination{};
    std::thread m_thread;
    bool m_running = false;
    std::string m_pendingReason;
    bool m_pendingManipulation = false;
    std::string m_sentResult = "Sent";
    std::string m_failedResult = "Failed";
    std::string m_socketErrorReason = "Socket error";
    std::string m_releaseReason = "Input released";
};

// All mutable UI/session state for the tool, held in a single global instance so
// the Win32 message handlers and the per-frame ImGui render code share one view.
struct AppState {
    FrameState frame = BuildResetTPose(MakeNeutralFrame());
    KeyboardInputState keyboard;
    UdpStreamer streamer;
    bool focused = true;
    bool captureActive = false;
    bool cursorHidden = false;
    POINT lastMouse{};
    DragSnapshot drag{};
    DeviceIndex dragDevice = DeviceIndex::Hmd;
    int captureButton = VK_LBUTTON;
    ManipulationFrame manipulationFrame = ManipulationFrame::Hmd;
    int captureModifiers = ManipulationModifier_None;
    bool mirrorHands = false;
    bool mirrorFeet = false;
    // Per-hand finger bends ([0] left, [1] right). The wheel adjusts all fingers,
    // or just the one whose number key is held (see kFingerKeys). lastFingerBends
    // tracks the previous values to detect wheel changes for the log.
    std::array<FingerBends, 2> fingerBends{};
    std::array<FingerBends, 2> lastFingerBends{};
    bool joystickActive = false;  // dragging the empty body panel as the right stick
    ImVec2 joystickOrigin{};      // press point (ImGui space) used as the stick center
    float joystickX = 0.0f;       // current right-stick deflection, [-1, 1]
    float joystickY = 0.0f;
    bool alwaysOnTop = false;
    UiMode uiMode = UiMode::Full;
    bool uiModeChangePending = false;
    UiMode pendingUiMode = UiMode::Full;
    bool disclaimerAccepted = false;
    bool driverStatusSet = false;
    DriverStatus driverStatus = DriverStatus::Failed;
    std::string driverStatusDetail;
    int selectedLogIndex = -1;
    bool logScrollToLatest = true;

    // MMD dance: dialog parameters, async Blender solve, and playback state.
    bool danceDialogOpen = false;
    char danceVmdPath[1024] = {};
    char danceModelPath[1024] = {};
    char danceBlenderPath[1024] = {};   // Advanced: blender.exe (auto-filled)
    char danceMmdToolsPath[1024] = {};  // Advanced: MMD Tools dir (auto-filled)
    // Fixed playback and remap defaults used by the dance dialog.
    float danceHeight = 1.5f;
    float danceSpeed = 1.0f;
    float danceFps = 60.0f;
    float danceHandReach = 1.22f;
    bool danceLoop = true;
    std::string danceStatus;
    bool danceConverting = false;
    std::future<MmdExportResult> danceFuture;
    DanceMotion danceMotion;
    bool dancePlaying = false;
    bool dancePaused = false;
    float dancePausedElapsed = 0.0f;
    std::chrono::steady_clock::time_point danceStartTime;
    float danceRootX = 0.0f;
    float danceRootZ = 0.0f;
};

AppState g_app;

void ApplyAlwaysOnTop(HWND hwnd, bool onTop);

std::string SettingsDirectory() {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData || localAppData[0] == '\0') {
        return ".";
    }
    return std::string(localAppData) + "\\AnyaDance";
}

std::string SettingsPath() {
    return SettingsDirectory() + "\\tool_state.ini";
}

const char* UiModeCode(UiMode mode) {
    return mode == UiMode::Mini ? "mini" : "full";
}

UiMode ParseUiMode(const std::string& value) {
    return value == "mini" ? UiMode::Mini : UiMode::Full;
}

int MinWindowWidth() {
    return g_app.uiMode == UiMode::Mini ? kMiniMinWindowWidth : kMinWindowWidth;
}

int MinWindowHeight() {
    return g_app.uiMode == UiMode::Mini ? kMiniMinWindowHeight : kMinWindowHeight;
}

void CopyPreferenceString(char* buffer, std::size_t size, const std::string& value) {
    if (size == 0) {
        return;
    }
    std::snprintf(buffer, size, "%s", value.c_str());
}

void LoadPreferences(HWND hwnd) {
    std::ifstream in(SettingsPath());
    std::string key;
    while (in >> key) {
        if (key == "language") {
            std::string value;
            in >> value;
            SetCurrentLanguage(FindLanguageByCode(value.c_str(), CurrentLanguage()));
        } else if (key == "disclaimer_accepted") {
            int value = 0;
            in >> value;
            g_app.disclaimerAccepted = value != 0;
        } else if (key == "always_on_top") {
            int value = 0;
            in >> value;
            g_app.alwaysOnTop = value != 0;
        } else if (key == "ui_mode") {
            std::string value;
            in >> value;
            g_app.uiMode = ParseUiMode(value);
        } else if (key == "dance_blender_path") {
            std::string value;
            in >> std::quoted(value);
            CopyPreferenceString(g_app.danceBlenderPath, sizeof(g_app.danceBlenderPath), value);
        } else if (key == "dance_mmd_tools_path") {
            std::string value;
            in >> std::quoted(value);
            CopyPreferenceString(g_app.danceMmdToolsPath, sizeof(g_app.danceMmdToolsPath), value);
        } else if (key == "window") {
            int x = 100;
            int y = 100;
            int w = kDefaultWindowWidth;
            int h = kDefaultWindowHeight;
            in >> x >> y >> w >> h;
            w = std::max(w, MinWindowWidth());
            h = std::max(h, MinWindowHeight());
            MoveWindow(hwnd, x, y, w, h, FALSE);
        }
    }
}

void SavePreferences(HWND hwnd) {
    const std::string dir = SettingsDirectory();
    CreateDirectoryA(dir.c_str(), nullptr);
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    std::ofstream out(SettingsPath(), std::ios::trunc);
    out << "language " << GetLanguageInfo(CurrentLanguage()).code << '\n';
    out << "disclaimer_accepted " << (g_app.disclaimerAccepted ? 1 : 0) << '\n';
    out << "always_on_top " << (g_app.alwaysOnTop ? 1 : 0) << '\n';
    out << "ui_mode " << UiModeCode(g_app.uiMode) << '\n';
    out << "dance_blender_path " << std::quoted(std::string(g_app.danceBlenderPath)) << '\n';
    out << "dance_mmd_tools_path " << std::quoted(std::string(g_app.danceMmdToolsPath)) << '\n';
    out << "window " << rect.left << ' ' << rect.top << ' ' << (rect.right - rect.left) << ' ' << (rect.bottom - rect.top) << '\n';
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    return D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_swapChain,
        &g_device,
        &featureLevel,
        &g_deviceContext) == S_OK;
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

void CleanupBodyBackgroundTexture() {
    if (g_bodyBackgroundTexture) {
        g_bodyBackgroundTexture->Release();
        g_bodyBackgroundTexture = nullptr;
    }
    g_bodyBackgroundWidth = 0;
    g_bodyBackgroundHeight = 0;
}

void CleanupFooterBannerTexture() {
    if (g_footerBannerTexture) {
        g_footerBannerTexture->Release();
        g_footerBannerTexture = nullptr;
    }
    g_footerBannerWidth = 0;
    g_footerBannerHeight = 0;
}

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_mainRenderTargetView);
    backBuffer->Release();
}

void CleanupDeviceD3D() {
    CleanupBodyBackgroundTexture();
    CleanupFooterBannerTexture();
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_deviceContext) { g_deviceContext->Release(); g_deviceContext = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

bool LoadPngResourceTexture(HINSTANCE instance, int resourceId, ID3D11ShaderResourceView** outTexture, int* outWidth, int* outHeight) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), MAKEINTRESOURCEW(10));
    if (!resource) {
        return false;
    }
    HGLOBAL loadedResource = LoadResource(instance, resource);
    const DWORD resourceSize = SizeofResource(instance, resource);
    const void* resourceData = LockResource(loadedResource);
    if (!loadedResource || resourceSize == 0 || !resourceData) {
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateStream(&stream);
    }
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<void*>(resourceData)),
            resourceSize);
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, &frame);
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(
            frame,
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr)) {
        hr = converter->GetSize(&width, &height);
    }
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    if (SUCCEEDED(hr)) {
        hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
    }
    if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = width * 4;

        ID3D11Texture2D* texture = nullptr;
        hr = g_device->CreateTexture2D(&desc, &data, &texture);
        if (SUCCEEDED(hr)) {
            hr = g_device->CreateShaderResourceView(texture, nullptr, outTexture);
            texture->Release();
        }
    }

    if (SUCCEEDED(hr)) {
        *outWidth = static_cast<int>(width);
        *outHeight = static_cast<int>(height);
        ok = true;
    }

    if (converter) { converter->Release(); }
    if (frame) { frame->Release(); }
    if (decoder) { decoder->Release(); }
    if (stream) { stream->Release(); }
    if (factory) { factory->Release(); }
    return ok;
}

// Draw the faint body silhouette behind the device boxes, centered and scaled
// to the panel height while preserving the image's aspect ratio.
void DrawBodyBackground(const ImVec2& panelMin, const ImVec2& panelSize) {
    if (!g_bodyBackgroundTexture || g_bodyBackgroundWidth <= 0 || g_bodyBackgroundHeight <= 0 || panelSize.x <= 0.0f || panelSize.y <= 0.0f) {
        return;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 panelMax(panelMin.x + panelSize.x, panelMin.y + panelSize.y);
    const float imageAspect = static_cast<float>(g_bodyBackgroundWidth) / static_cast<float>(g_bodyBackgroundHeight);
    const float drawHeight = panelSize.y;
    const float drawWidth = drawHeight * imageAspect;
    const ImVec2 imageMin(panelMin.x + (panelSize.x - drawWidth) * 0.5f, panelMin.y);
    const ImVec2 imageMax(imageMin.x + drawWidth, imageMin.y + drawHeight);
    draw->PushClipRect(panelMin, panelMax, true);
    draw->AddImage(
        reinterpret_cast<ImTextureID>(g_bodyBackgroundTexture),
        imageMin,
        imageMax,
        ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f),
        IM_COL32(255, 255, 255, 92));
    draw->PopClipRect();
}

float DrawFooterBanner(const ImVec2& footerMin, const ImVec2& footerSize) {
    if (!g_footerBannerTexture || g_footerBannerWidth <= 0 || g_footerBannerHeight <= 0 || footerSize.x <= 0.0f || footerSize.y <= 0.0f) {
        return footerMin.x + footerSize.x;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float imageAspect = static_cast<float>(g_footerBannerWidth) / static_cast<float>(g_footerBannerHeight);
    const float drawHeight = footerSize.y;
    const float drawWidth = drawHeight * imageAspect;
    const ImVec2 imageMin(footerMin.x + footerSize.x - drawWidth, footerMin.y);
    const ImVec2 imageMax(footerMin.x + footerSize.x, footerMin.y + drawHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(footerMin, ImVec2(footerMin.x + footerSize.x, footerMin.y + footerSize.y), true);
    draw->AddImage(
        reinterpret_cast<ImTextureID>(g_footerBannerTexture),
        imageMin,
        imageMax,
        ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f),
        IM_COL32(255, 255, 255, 120));
    draw->PopClipRect();
    return imageMin.x - style.ItemSpacing.x;
}

bool FingerBendsEqual(const FingerBends& lhs, const FingerBends& rhs) {
    return lhs.thumb == rhs.thumb &&
        lhs.index == rhs.index &&
        lhs.middle == rhs.middle &&
        lhs.ring == rhs.ring &&
        lhs.pinky == rhs.pinky;
}

bool ControllerEqual(const ControllerState& lhs, const ControllerState& rhs) {
    return lhs.trigger_click == rhs.trigger_click &&
        lhs.trigger_value == rhs.trigger_value &&
        lhs.menu_click == rhs.menu_click &&
        lhs.system_click == rhs.system_click &&
        lhs.a_click == rhs.a_click &&
        lhs.b_click == rhs.b_click &&
        lhs.grip_click == rhs.grip_click &&
        lhs.grip_value == rhs.grip_value &&
        lhs.joystick_x == rhs.joystick_x &&
        lhs.joystick_y == rhs.joystick_y &&
        lhs.trackpad_x == rhs.trackpad_x &&
        lhs.trackpad_y == rhs.trackpad_y &&
        lhs.has_finger_bends == rhs.has_finger_bends &&
        FingerBendsEqual(lhs.finger_bends, rhs.finger_bends);
}

bool ControllerChanged(const std::array<ControllerState, 2>& lhs, const std::array<ControllerState, 2>& rhs) {
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (!ControllerEqual(lhs[i], rhs[i])) {
            return true;
        }
    }
    return false;
}

void ReleaseMouseCapture();

void HandleFocusLost() {
    const auto previous = g_app.frame.controllers;
    ReleaseMouseCapture();
    // End any body-panel stick drag; NeutralizeControllerInputs recenters it.
    g_app.joystickActive = false;
    g_app.joystickX = 0.0f;
    g_app.joystickY = 0.0f;
    g_app.focused = false;
    g_app.keyboard.SetFocus(false);
    NeutralizeControllerInputs(g_app.frame);
    if (ControllerChanged(previous, g_app.frame.controllers)) {
        g_app.streamer.UpdateFrame(g_app.frame, En(Text::ReleaseReason), false);
    }
}

void ReleaseMouseCapture() {
    if (!g_app.captureActive) {
        return;
    }
    SetCursorPos(g_app.lastMouse.x, g_app.lastMouse.y);
    ReleaseCapture();
    if (g_app.cursorHidden) {
        while (ShowCursor(TRUE) < 0) {}
        g_app.cursorHidden = false;
    }
    g_app.captureActive = false;
}

bool IsKeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Pin or unpin the window above all others without moving, resizing, or
// stealing focus, so the "Always on top" checkbox keeps it visible over a game.
void ApplyAlwaysOnTop(HWND hwnd, bool onTop) {
    SetWindowPos(hwnd, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ApplyUiMode(HWND hwnd, UiMode mode) {
    g_app.uiMode = mode;
    g_app.pendingUiMode = mode;
    g_app.uiModeChangePending = false;
    RECT rect{};
    if (GetWindowRect(hwnd, &rect)) {
        const int currentW = rect.right - rect.left;
        const int currentH = rect.bottom - rect.top;
        const int targetW = mode == UiMode::Mini ? kMiniMinWindowWidth : std::max(currentW, kMinWindowWidth);
        const int targetH = mode == UiMode::Mini ? kMiniMinWindowHeight : std::max(currentH, kMinWindowHeight);
        SetWindowPos(hwnd, nullptr, rect.left, rect.top, targetW, targetH,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

void SetUiMode(UiMode mode) {
    if (g_app.uiMode == mode || (g_app.uiModeChangePending && g_app.pendingUiMode == mode)) {
        return;
    }
    g_app.pendingUiMode = mode;
    g_app.uiModeChangePending = true;
}

void ApplyPendingUiMode(HWND hwnd) {
    if (g_app.uiModeChangePending) {
        ApplyUiMode(hwnd, g_app.pendingUiMode);
    }
}

void RenderUiModeControls(HWND hwnd, bool includeAlwaysOnTop) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s:", Tr(Text::UiModeLabel));
    ImGui::SameLine();
    if (ImGui::RadioButton(Tr(Text::UiModeFull), g_app.uiMode == UiMode::Full)) {
        SetUiMode(UiMode::Full);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(Tr(Text::UiModeMini), g_app.uiMode == UiMode::Mini)) {
        SetUiMode(UiMode::Mini);
    }
    if (includeAlwaysOnTop) {
        ImGui::SameLine();
        if (ImGui::Checkbox(Tr(Text::AlwaysOnTop), &g_app.alwaysOnTop)) {
            ApplyAlwaysOnTop(hwnd, g_app.alwaysOnTop);
        }
    }
}

int ModifiersForCaptureButton(int captureButton) {
    const bool rightDown = IsKeyDown(VK_RBUTTON);
    if (captureButton == VK_MBUTTON) {
        return ManipulationModifier_Ctrl | (rightDown ? ManipulationModifier_Shift : 0);
    }
    if (captureButton == VK_LBUTTON && rightDown) {
        return ManipulationModifier_Shift;
    }
    return ManipulationModifier_None;
}

void BeginMouseCapture(HWND hwnd, DeviceIndex device, int captureButton) {
    g_app.captureActive = true;
    g_app.dragDevice = device;
    g_app.captureButton = captureButton;
    g_app.captureModifiers = ModifiersForCaptureButton(captureButton);
    g_app.drag = BeginDrag(g_app.frame, device);
    GetCursorPos(&g_app.lastMouse);
    SetCapture(hwnd);
    while (ShowCursor(FALSE) >= 0) {}
    g_app.cursorHidden = true;
}

bool MirrorEnabledFor(DeviceIndex device) {
    if (device == DeviceIndex::LeftController || device == DeviceIndex::RightController) {
        return g_app.mirrorHands;
    }
    if (device == DeviceIndex::LeftFoot || device == DeviceIndex::RightFoot) {
        return g_app.mirrorFeet;
    }
    return false;
}

int CurrentModifiers() {
    return ModifiersForCaptureButton(g_app.captureButton);
}

void UpdateCapture(HWND hwnd) {
    if (!g_app.captureActive) {
        return;
    }
    const bool focused = GetForegroundWindow() == hwnd;
    const bool captureButtonDown = IsKeyDown(g_app.captureButton);
    const bool escapeDown = IsKeyDown(VK_ESCAPE);
    if (!focused || !captureButtonDown || escapeDown) {
        ReleaseMouseCapture();
        return;
    }

    POINT current{};
    GetCursorPos(&current);
    const float dx = static_cast<float>(current.x - g_app.lastMouse.x);
    const float dy = static_cast<float>(current.y - g_app.lastMouse.y);
    if (current.x != g_app.lastMouse.x || current.y != g_app.lastMouse.y) {
        SetCursorPos(g_app.lastMouse.x, g_app.lastMouse.y);
    }
    const int modifiers = CurrentModifiers();
    if (modifiers != g_app.captureModifiers) {
        g_app.drag = BeginDrag(g_app.frame, g_app.dragDevice);
        g_app.captureModifiers = modifiers;
    }
    if (dx != 0.0f || dy != 0.0f) {
        ApplyDragDelta(g_app.drag, g_app.frame, dx, dy, modifiers, g_app.manipulationFrame);
        DeviceIndex mirroredDevice = DeviceIndex::Hmd;
        if (MirrorEnabledFor(g_app.dragDevice) && MirroredDeviceFor(g_app.dragDevice, mirroredDevice)) {
            ApplySymmetricMirror(g_app.drag, g_app.frame, mirroredDevice, g_app.manipulationFrame);
        }
        // Name the dragged device so the log distinguishes, e.g., "Hip manipulated".
        const std::string reason =
            std::string(DeviceName(DeviceSlot(g_app.dragDevice), Language::English)) + " manipulated";
        g_app.streamer.UpdateFrame(g_app.frame, reason, true);
    }
}

void ApplyFingerBend(FrameState& frame) {
    for (std::size_t i = 0; i < frame.controllers.size(); ++i) {
        frame.controllers[i].has_finger_bends = true;
        frame.controllers[i].finger_bends = g_app.fingerBends[i];
    }
}

FrameState FrameWithCurrentFingerBends() {
    FrameState frame = g_app.frame;
    ApplyFingerBend(frame);
    return frame;
}

// Drive the right controller's thumbstick (and matching trackpad) directly, used
// by the body-panel stick drag. controllers[1] is the right controller.
void ApplyRightJoystick(FrameState& frame, float x, float y) {
    ControllerState& right = frame.controllers[1];
    right.joystick_x = x;
    right.joystick_y = y;
    right.trackpad_x = x;
    right.trackpad_y = y;
}

// The physical key name and the VR action it drives, kept separate so a log
// entry can read "E down (turn right)".
struct KeyInfo {
    const char* key;
    const char* action;
};

KeyInfo DescribeKey(ToolKey key) {
    switch (key) {
    case ToolKey::W: return {"W", "forward"};
    case ToolKey::A: return {"A", "left"};
    case ToolKey::S: return {"S", "back"};
    case ToolKey::D: return {"D", "right"};
    case ToolKey::Q: return {"Q", "turn left"};
    case ToolKey::E: return {"E", "turn right"};
    case ToolKey::Space: return {"Space", "jump"};
    case ToolKey::M: return {"M", "menu"};
    case ToolKey::V: return {"V", "voice"};
    case ToolKey::Z: return {"Z", "left trigger"};
    case ToolKey::X: return {"X", "right trigger"};
    case ToolKey::Count: break;
    }
    return {"Key", ""};
}

void HandleKeyboard(HWND hwnd) {
    // Track focus so releasing keys/inputs is reported when the window loses it.
    const bool focused = GetForegroundWindow() == hwnd;
    const bool textEditing = ImGui::GetIO().WantTextInput;
    if (focused != g_app.focused) {
        if (!focused) {
            HandleFocusLost();
        } else {
            g_app.focused = true;
            g_app.keyboard.SetFocus(true);
        }
    }
    g_app.keyboard.SetTextEditing(textEditing);
    if (!focused || textEditing) {
        return;
    }

    struct Binding { int vk; ToolKey key; };
    const Binding bindings[] = {
        {'W', ToolKey::W}, {'A', ToolKey::A}, {'S', ToolKey::S}, {'D', ToolKey::D},
        {'Q', ToolKey::Q}, {'E', ToolKey::E}, {VK_SPACE, ToolKey::Space}, {'M', ToolKey::M},
        {'V', ToolKey::V}, {'Z', ToolKey::Z}, {'X', ToolKey::X},
    };

    // Name each key whose state changed this frame, e.g. "W down (forward),
    // Z up (left trigger)", so the log says exactly what happened. Every key maps
    // straight to its button/axis, so a key edge is exactly the input edge.
    std::string reason;
    for (const Binding& binding : bindings) {
        const bool down = (GetAsyncKeyState(binding.vk) & 0x8000) != 0;
        if (g_app.keyboard.HandleKey(binding.key, down, false)) {
            if (!reason.empty()) {
                reason += ", ";
            }
            const KeyInfo info = DescribeKey(binding.key);
            reason += info.key;
            reason += down ? " down (" : " up (";
            reason += info.action;
            reason += ")";
        }
    }

    const auto previous = g_app.frame.controllers;
    g_app.keyboard.UpdateFrameInputs(g_app.frame);
    // While a dance plays it owns the finger bends. UpdateDancePlayback merges
    // keyboard inputs and streams the final frame. When paused, the dance loop
    // returns early, so we apply finger bends here so they are not lost from
    // the controller frame that UpdateFrameInputs just cleared.
    if (!g_app.dancePlaying || g_app.dancePaused) {
        ApplyFingerBend(g_app.frame);
    }
    // The keyboard rebuild above resets the right stick to the Q/E turn axis, so
    // re-apply an in-progress body-panel stick drag (updated each frame in
    // RenderBodyPanel) to keep it from snapping back to neutral.
    if (g_app.joystickActive) {
        ApplyRightJoystick(g_app.frame, g_app.joystickX, g_app.joystickY);
    }

    // A mouse-wheel finger bend leaves no key edge, so report it with its value.
    // Name the single finger when only one changed; otherwise call it "Fingers".
    if (reason.empty()) {
        int changedCount = 0;
        const FingerKey* changed = nullptr;
        for (const FingerKey& key : kFingerKeys) {
            if (FingerBendValue(g_app.fingerBends[key.hand], key.finger) !=
                FingerBendValue(g_app.lastFingerBends[key.hand], key.finger)) {
                ++changedCount;
                changed = &key;
            }
        }
        if (changedCount == 1) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%s %.0f%%", changed->name,
                          FingerBendValue(g_app.fingerBends[changed->hand], changed->finger) * 100.0f);
            reason = buf;
        } else if (changedCount > 1) {
            reason = "Fingers";
        }
    }
    g_app.lastFingerBends[0] = g_app.fingerBends[0];
    g_app.lastFingerBends[1] = g_app.fingerBends[1];

    // Any remaining controller change with no attributed cause falls back to the
    // generic reason (defensive; the cases above normally cover every change).
    // When playing (not paused), UpdateDancePlayback is the sole streamer to avoid
    // dueling finger packets. When paused, UpdateDancePlayback returns early, so
    // stream controller changes here to keep joystick/WASD movement working.
    if ((!g_app.dancePlaying || g_app.dancePaused) && (!reason.empty() || ControllerChanged(previous, g_app.frame.controllers))) {
        if (reason.empty()) {
            reason = En(Text::KeyboardReason);
        }
        g_app.streamer.UpdateFrame(g_app.frame, reason, false);
    }
}

// One-line summaries shown inside each device box: position (x y z) and the
// orientation quaternion laid out as a single x y z w row.
std::string PoseSummary(const DeviceState& device) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "P %.2f %.2f %.2f", device.position.x, device.position.y, device.position.z);
    return buf;
}

std::string RotationSummary(const DeviceState& device) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Q %.2f %.2f %.2f %.2f",
                  device.rotation.x, device.rotation.y, device.rotation.z, device.rotation.w);
    return buf;
}

// Draw one device's box and start a mouse drag when it is left- or middle-clicked,
// so the box doubles as a grab handle.
void DeviceBox(HWND hwnd, DeviceIndex deviceIndex, ImVec2 size, bool miniMode = false) {
    const std::size_t slot = DeviceSlot(deviceIndex);
    DeviceState& device = g_app.frame.devices[slot];
    ImGui::InvisibleButton(kDevices[slot].id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool leftClicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool middleClicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 bg = IM_COL32(38, 43, 48, 128);
    const ImU32 border = hovered ? IM_COL32(76, 154, 255, 255) : IM_COL32(83, 91, 102, 255);
    draw->AddRectFilled(min, max, bg, 6.0f);
    draw->AddRect(min, max, border, 6.0f, 0, hovered ? 2.0f : 1.0f);
    ImVec2 textPos = ImVec2(min.x + 10.0f, min.y + 8.0f);
    draw->AddText(textPos, IM_COL32(245, 247, 250, 255), DeviceName(slot));
    textPos.y += 20.0f;
    if (!miniMode) {
        const std::string pos = PoseSummary(device);
        draw->AddText(textPos, IM_COL32(198, 205, 214, 255), pos.c_str());
        textPos.y += 18.0f;
        const std::string rot = RotationSummary(device);
        draw->AddText(textPos, IM_COL32(198, 205, 214, 255), rot.c_str());
        textPos.y += 18.0f;
    }
    if (g_app.captureActive && g_app.dragDevice == deviceIndex) {
        draw->AddText(textPos, IM_COL32(107, 203, 119, 255), Tr(Text::Capture));
        textPos.y += 18.0f;
    }
    if (device.position.y >= kMaxDeviceY || device.y_clamped) {
        draw->AddText(ImVec2(max.x - 56.0f, min.y + 8.0f), IM_COL32(255, 196, 87, 255), Tr(Text::YMax));
    }
    if (!miniMode && deviceIndex == DeviceIndex::Hmd) {
        draw->AddText(textPos, IM_COL32(164, 174, 187, 255), Tr(Text::HmdHelp));
    }
    if (leftClicked) {
        BeginMouseCapture(hwnd, deviceIndex, VK_LBUTTON);
    } else if (middleClicked) {
        BeginMouseCapture(hwnd, deviceIndex, VK_MBUTTON);
    }
}

void MirrorCheckbox(const char* id, bool* value, float rowHeight) {
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(1.0f, rowHeight * 0.34f));
    ImGui::PushID(id);
    ImGui::Checkbox(Tr(Text::Mirror), value);
    ImGui::PopID();
    ImGui::EndGroup();
}

// Left half of the window: the mouse-help line and the six device boxes laid out
// over the body silhouette (HMD centered, controllers and feet paired with a
// Mirror checkbox between them, hip centered).
void RenderBodyPanel(HWND hwnd, bool miniMode = false) {
    const float availableW = ImGui::GetContentRegionAvail().x;
    float panelW = availableW;
    if (!miniMode) {
        const float logMinW = 330.0f;
        panelW = std::clamp(availableW * 0.46f, 450.0f, 620.0f);
        panelW = std::min(panelW, std::max(450.0f, availableW - logMinW));
    }

    ImGui::BeginChild("body", ImVec2(panelW, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 bodyPanelMin = ImGui::GetWindowPos();
    const ImVec2 bodyPanelSize = ImGui::GetWindowSize();
    DrawBodyBackground(bodyPanelMin, bodyPanelSize);

    // The mouse wheel over the body bends the fingers. IsWindowHovered respects
    // z-order, so a log detail window on top owns wheel scrolling for its JSON
    // view. The body child uses NoScrollWithMouse, making this the sole body
    // wheel path.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseWheel != 0.0f && !io.WantTextInput &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        // Scroll up opens (decreases bend), scroll down closes, matching the prior
        // all-finger behavior. Holding number keys narrows the wheel to just those
        // fingers; otherwise every finger on both hands moves together.
        const float increment = -io.MouseWheel * kFingerBendStep;
        bool anyFingerKey = false;
        for (const FingerKey& key : kFingerKeys) {
            if (IsKeyDown(key.vk)) {
                AdjustFingerBend(g_app.fingerBends[key.hand], key.finger, increment);
                anyFingerKey = true;
            }
        }
        if (!anyFingerKey) {
            AdjustAllFingerBends(g_app.fingerBends[0], g_app.fingerBends[1], increment);
        }
    }
    if (!miniMode) {
        const float instructionH = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        ImGui::BeginChild("mouse_help", ImVec2(panelW - 12.0f, instructionH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + panelW - 12.0f);
        ImGui::TextUnformatted(Tr(Text::MouseHelp));
        ImGui::PopTextWrapPos();
        ImGui::EndChild();

        // Manipulation frame: HMD moves/rotates along the head heading, Global along
        // fixed world axes. Vertical translation is world up either way.
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Tr(Text::FrameLabel));
        ImGui::SameLine();
        if (ImGui::RadioButton(Tr(Text::FrameHmd), g_app.manipulationFrame == ManipulationFrame::Hmd)) {
            g_app.manipulationFrame = ManipulationFrame::Hmd;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(Tr(Text::FrameGlobal), g_app.manipulationFrame == ManipulationFrame::Global)) {
            g_app.manipulationFrame = ManipulationFrame::Global;
        }
    }

    const float rowsTopY = ImGui::GetCursorPosY() + (miniMode ? 4.0f : 10.0f);
    const float rowsAvailH = std::max(0.0f, ImGui::GetContentRegionAvail().y - 10.0f);
    const float pairGap = 8.0f;
    const float rowGap = std::clamp(rowsAvailH * 0.018f, 8.0f, 14.0f);
    const float boxH = std::max(1.0f, (rowsAvailH - rowGap * 3.0f) * 0.25f);
    const float mirrorW = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(Tr(Text::Mirror)).x;
    const float minBoxW = miniMode ? 72.0f : 150.0f;
    const float maxBoxW = std::max(minBoxW, std::min(240.0f, (panelW - mirrorW - pairGap * 2.0f) * 0.5f));
    const float boxW = std::clamp((panelW - mirrorW - pairGap * 2.0f) * 0.5f, minBoxW, maxBoxW);
    const float pairRowW = boxW * 2.0f + mirrorW + pairGap * 2.0f;
    const float pairLeftX = std::max(0.0f, (panelW - pairRowW) * 0.5f);
    const float centerX = std::max(0.0f, (panelW - boxW) * 0.5f);
    const auto rowY = [rowsTopY, boxH, rowGap](int row) {
        return rowsTopY + static_cast<float>(row) * (boxH + rowGap);
    };

    ImGui::SetCursorPos(ImVec2(centerX, rowY(0)));
    DeviceBox(hwnd, DeviceIndex::Hmd, ImVec2(boxW, boxH), miniMode);
    ImGui::SetCursorPos(ImVec2(pairLeftX, rowY(1)));
    DeviceBox(hwnd, DeviceIndex::LeftController, ImVec2(boxW, boxH), miniMode);
    ImGui::SameLine(0.0f, pairGap);
    MirrorCheckbox("hands", &g_app.mirrorHands, boxH);
    ImGui::SameLine(0.0f, pairGap);
    DeviceBox(hwnd, DeviceIndex::RightController, ImVec2(boxW, boxH), miniMode);
    ImGui::SetCursorPos(ImVec2(centerX, rowY(2)));
    DeviceBox(hwnd, DeviceIndex::Hip, ImVec2(boxW, boxH), miniMode);
    ImGui::SetCursorPos(ImVec2(pairLeftX, rowY(3)));
    DeviceBox(hwnd, DeviceIndex::LeftFoot, ImVec2(boxW, boxH), miniMode);
    ImGui::SameLine(0.0f, pairGap);
    MirrorCheckbox("feet", &g_app.mirrorFeet, boxH);
    ImGui::SameLine(0.0f, pairGap);
    DeviceBox(hwnd, DeviceIndex::RightFoot, ImVec2(boxW, boxH), miniMode);
    ImGui::SetCursorPosY(rowY(3) + boxH);

    // Empty area of the body panel acts as the right thumbstick: left-press sets
    // the neutral center, then dragging deflects the stick within [-1, 1]. This
    // lets the user aim the right-hand quick menu (opened by holding M) and pick
    // items. The ImGui Win32 backend captures the mouse while a button is held,
    // so the drag keeps tracking even when the cursor leaves the panel.
    const bool overEmptyBody = ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
    if (!g_app.captureActive && !g_app.joystickActive && overEmptyBody &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_app.joystickActive = true;
        g_app.joystickOrigin = ImGui::GetIO().MousePos;
        g_app.joystickX = 0.0f;
        g_app.joystickY = 0.0f;
    }
    if (g_app.joystickActive) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_app.joystickActive = false;
            g_app.joystickX = 0.0f;
            g_app.joystickY = 0.0f;
            ApplyRightJoystick(g_app.frame, 0.0f, 0.0f);
            g_app.streamer.UpdateFrame(g_app.frame, "Right stick release", false);
        } else {
            // Deflection from the press point; screen Y grows downward, so negate
            // it to make dragging up push the stick forward (+Y).
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float x = std::clamp((mouse.x - g_app.joystickOrigin.x) / kJoystickDragRangePixels, -1.0f, 1.0f);
            const float y = std::clamp((g_app.joystickOrigin.y - mouse.y) / kJoystickDragRangePixels, -1.0f, 1.0f);
            if (x != g_app.joystickX || y != g_app.joystickY) {
                g_app.joystickX = x;
                g_app.joystickY = y;
                ApplyRightJoystick(g_app.frame, x, y);
                g_app.streamer.UpdateFrame(g_app.frame, "Right stick", true);
            }
            // Guide overlay: the range ring at the press point and a knob at the
            // current deflection.
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 origin = g_app.joystickOrigin;
            const ImVec2 knob(origin.x + g_app.joystickX * kJoystickDragRangePixels,
                              origin.y - g_app.joystickY * kJoystickDragRangePixels);
            draw->AddCircle(origin, kJoystickDragRangePixels, IM_COL32(118, 161, 220, 130), 0, 1.5f);
            draw->AddLine(origin, knob, IM_COL32(118, 161, 220, 160), 1.5f);
            draw->AddCircleFilled(knob, 7.0f, IM_COL32(107, 203, 119, 235));
        }
    }
    ImGui::EndChild();
}

// Hover preview for a log row: timestamp, endpoint, reason/result, and the
// pretty-printed JSON payload in the monospace font.
void RenderLogTooltip(const UdpLogEntry& entry) {
    ImGui::BeginTooltip();
    ImGui::Text("%s", entry.timeText.c_str());
    ImGui::Text("127.0.0.1:%u", kUdpPort);
    if (!entry.detail.empty()) {
        ImGui::TextWrapped("%s", entry.detail.c_str());
    } else {
        ImGui::TextWrapped("%s", entry.result.c_str());
    }
    ImGui::Separator();
    ImGui::BeginChild("json_tip", ImVec2(640, 320), true, ImGuiWindowFlags_HorizontalScrollbar);
    const std::string pretty = PrettyPrintJson(entry.payload);
    if (g_monoFont) { ImGui::PushFont(g_monoFont); }
    ImGui::TextUnformatted(pretty.c_str());
    if (g_monoFont) { ImGui::PopFont(); }
    ImGui::EndChild();
    ImGui::EndTooltip();
}

// curl has no UDP transport, so a runnable "resend" command for this loopback
// UDP datagram is a dependency-free PowerShell one-liner the user already has.
std::string BuildResendCommand(const std::string& payload) {
    std::string escaped;
    escaped.reserve(payload.size());
    for (const char c : payload) {
        escaped.push_back(c);
        if (c == '\'') {
            escaped.push_back('\''); // double single quotes for a PS literal string
        }
    }
    std::string command = "$c=New-Object System.Net.Sockets.UdpClient;";
    command += "$b=[Text.Encoding]::UTF8.GetBytes('" + escaped + "');";
    command += "$c.Send($b,$b.Length,'127.0.0.1'," + std::to_string(kUdpPort) + ")>$null;";
    command += "$c.Close()";
    return command;
}

// Floating detail window for the selected log entry: header fields, the three
// payload actions (copy raw, copy a PowerShell resend command, resend in-tool),
// and the full pretty-printed JSON. Closing it clears the selection.
void RenderPinnedLogDetail(const UdpLogEntry& entry) {
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(720.0f, 440.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UDP Log Detail", &open)) {
        ImGui::End();
        if (!open) {
            g_app.selectedLogIndex = -1;
        }
        return;
    }
    if (!open) {
        g_app.selectedLogIndex = -1;
    }
    ImGui::Text("%s", entry.timeText.c_str());
    ImGui::Text("127.0.0.1:%u", kUdpPort);
    ImGui::Text("%s: %s", Tr(Text::Reason), entry.reason.c_str());
    if (!entry.detail.empty()) {
        ImGui::TextWrapped("%s", entry.detail.c_str());
    } else {
        ImGui::Text("%s: %s", Tr(Text::Result), entry.result.c_str());
    }
    // Copy the raw request body so it can be inspected or edited elsewhere.
    if (ImGui::Button(Tr(Text::Copy))) {
        ImGui::SetClipboardText(entry.payload.c_str());
    }
    ImGui::SameLine();
    // Copy a ready-to-run command that resends this datagram to the UDP endpoint.
    if (ImGui::Button(Tr(Text::CopyCommand))) {
        ImGui::SetClipboardText(BuildResendCommand(entry.payload).c_str());
    }
    ImGui::SameLine();
    // Resend this exact datagram to the UDP endpoint from the tool itself.
    if (ImGui::Button(Tr(Text::Resend))) {
        g_app.streamer.SendRaw(entry.payload, En(Text::ResendReason));
    }
    ImGui::Separator();
    ImGui::BeginChild("pinned_json", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
    const std::string pretty = PrettyPrintJson(entry.payload);
    if (g_monoFont) { ImGui::PushFont(g_monoFont); }
    ImGui::TextUnformatted(pretty.c_str());
    if (g_monoFont) { ImGui::PopFont(); }
    ImGui::EndChild();
    ImGui::End();
}

// Right half of the window: the scrolling table of UDP log entries. Rows show a
// tooltip on hover and open the pinned detail window when selected; the view
// follows the newest row only while it is already scrolled to the bottom, so
// scrolling up to inspect or click an older entry is not yanked back down.
void RenderLogPanel() {
    ImGui::BeginChild("log", ImVec2(0, 0), false);
    ImGui::TextUnformatted(Tr(Text::UdpLog));
    // Right-align the checkbox and Clear button: Clear pins to the right edge and
    // the checkbox sits just left of it. A checkbox spans the box (frame height)
    // plus the inner spacing and its label.
    const ImGuiStyle& logStyle = ImGui::GetStyle();
    const float clearX = ImGui::GetWindowContentRegionMax().x - 50.0f;
    const float checkboxWidth = ImGui::GetFrameHeight() + logStyle.ItemInnerSpacing.x +
        ImGui::CalcTextSize(Tr(Text::LogScrollLatest)).x;
    ImGui::SameLine(clearX - logStyle.ItemSpacing.x - checkboxWidth);
    ImGui::Checkbox(Tr(Text::LogScrollLatest), &g_app.logScrollToLatest);
    ImGui::SameLine(clearX);
    if (ImGui::SmallButton(Tr(Text::Clear))) {
        g_app.streamer.ClearLog();
        g_app.selectedLogIndex = -1;
    }

    const std::vector<UdpLogEntry> entries = g_app.streamer.SnapshotLog();
    if (g_app.selectedLogIndex >= static_cast<int>(entries.size())) {
        g_app.selectedLogIndex = -1;
    }

    if (ImGui::BeginTable("udp_log_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, -1))) {
        ImGui::TableSetupColumn(Tr(Text::Time), ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn(Tr(Text::Reason), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(Tr(Text::Result), ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const UdpLogEntry& entry = entries[static_cast<std::size_t>(i)];
            const bool selected = g_app.selectedLogIndex == i;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            if (ImGui::Selectable(entry.timeText.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing()))) {
                g_app.selectedLogIndex = selected ? -1 : i;
                g_app.logScrollToLatest = false;  // inspecting an entry stops following the tail
            }
            const bool rowHovered = ImGui::IsItemHovered();
            if (rowHovered && !selected) {
                RenderLogTooltip(entry);
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry.reason.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(entry.result.c_str());
        }
        // Follow the newest row while "Scroll to latest" is checked. Clicking a
        // log entry unchecks it (see above), so inspecting an older row is not
        // yanked back down; re-checking the box resumes tailing.
        if (g_app.logScrollToLatest && !entries.empty()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (g_app.selectedLogIndex >= 0 && g_app.selectedLogIndex < static_cast<int>(entries.size())) {
        RenderPinnedLogDetail(entries[static_cast<std::size_t>(g_app.selectedLogIndex)]);
    }
}

// Map a driver registration/restart outcome to its localized status string.
Text DriverStatusText(DriverStatus status) {
    switch (status) {
    case DriverStatus::Registered: return Text::StatusRegistered;
    case DriverStatus::Unregistered: return Text::StatusUnregistered;
    case DriverStatus::ManifestMissing: return Text::StatusManifestMissing;
    case DriverStatus::DriverDllMissing: return Text::StatusDriverDllMissing;
    case DriverStatus::OpenvrPathsMissing: return Text::StatusOpenvrPathsMissing;
    case DriverStatus::ConfigWriteFailed: return Text::StatusConfigWriteFailed;
    case DriverStatus::Restarting: return Text::StatusRestarting;
    case DriverStatus::RestartFailed: return Text::StatusRestartFailed;
    case DriverStatus::Failed: break;
    }
    return Text::StatusFailed;
}

// Kick off the Blender solve on a worker thread so the UI keeps rendering while
// Blender runs. The result is picked up by PollDanceExport.
void StartDanceExport() {
    if (g_app.danceConverting) {
        return;
    }
    MmdDanceConfig config;
    config.vmdPath = g_app.danceVmdPath;
    config.modelPath = g_app.danceModelPath;
    config.blenderPath = g_app.danceBlenderPath;
    config.mmdToolsPath = g_app.danceMmdToolsPath;
    config.fps = g_app.danceFps;
    g_app.danceConverting = true;
    g_app.danceStatus = Tr(Text::DanceConverting);
    g_app.danceFuture = std::async(std::launch::async, [config] { return RunMmdExport(config); });
}

float ClampDanceElapsed(float elapsed) {
    if (!g_app.danceMotion.valid || g_app.danceMotion.duration <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(elapsed, 0.0f, g_app.danceMotion.duration);
}

float DanceTimelineElapsed() {
    if (!g_app.danceMotion.valid || g_app.danceMotion.duration <= 0.0f) {
        return 0.0f;
    }
    if (g_app.dancePaused || !g_app.dancePlaying) {
        return ClampDanceElapsed(g_app.dancePausedElapsed);
    }
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - g_app.danceStartTime).count() * g_app.danceSpeed;
    if (g_app.danceLoop) {
        return std::fmod(std::max(0.0f, elapsed), g_app.danceMotion.duration);
    }
    return ClampDanceElapsed(elapsed);
}

void SetDanceStartForElapsed(float elapsed) {
    const float speed = g_app.danceSpeed != 0.0f ? g_app.danceSpeed : 1.0f;
    const auto offset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(elapsed / speed));
    g_app.danceStartTime = std::chrono::steady_clock::now() - offset;
}

void ApplyDanceFrameAt(float elapsed) {
    if (!g_app.danceMotion.valid) {
        return;
    }
    FrameState danceFrame = SampleDanceMotion(g_app.danceMotion, elapsed, g_app.danceLoop);
    AnchorDanceFrame(danceFrame, g_app.danceRootX, g_app.danceRootZ);

    const std::array<ControllerState, 2> danceControllers = danceFrame.controllers;
    // Keep keyboard-driven inputs (joystick turn, triggers) set this frame by
    // HandleKeyboard; the dance only owns poses and finger bends. Mirroring the
    // dance fingers into g_app.fingerBends keeps that store the single source of
    // truth, so Save Pose and the paused stream capture the dance's hands instead
    // of the stale wheel value.
    danceFrame.controllers = g_app.frame.controllers;
    ApplyDanceFingerBends(danceControllers, danceFrame.controllers, g_app.fingerBends);
    ClampFrameY(danceFrame);
    g_app.frame = danceFrame;
    g_app.streamer.UpdateFrame(g_app.frame);  // no reason: keep the 60 Hz log quiet
}

void SeekDancePlayback(float elapsed) {
    if (!g_app.danceMotion.valid) {
        return;
    }
    elapsed = ClampDanceElapsed(elapsed);
    g_app.dancePausedElapsed = elapsed;
    if (g_app.dancePlaying && !g_app.dancePaused) {
        SetDanceStartForElapsed(elapsed);
    }
    // Root anchor is owned by StartDancePlayback; seeking never changes it.
    // Re-rooting here from g_app.frame would accumulate because the frame already
    // has the previous root baked in via AnchorDanceFrame.
    ApplyDanceFrameAt(elapsed);
}

void StartDancePlayback() {
    if (!g_app.danceMotion.valid) {
        return;
    }
    const float startElapsed = ClampDanceElapsed(g_app.dancePausedElapsed);
    g_app.dancePlaying = true;
    g_app.dancePaused = false;
    SetDanceStartForElapsed(startElapsed);
    // Anchor the dance where the HMD currently stands so the avatar dances in place.
    const DeviceState& hmd = g_app.frame.devices[DeviceSlot(DeviceIndex::Hmd)];
    g_app.danceRootX = hmd.position.x;
    g_app.danceRootZ = hmd.position.z;
}

// Freeze playback on the current pose. The elapsed offset is remembered so Resume
// can pick the clock back up from exactly here.
void PauseDancePlayback() {
    if (!g_app.dancePlaying || g_app.dancePaused) {
        return;
    }
    g_app.dancePausedElapsed = DanceTimelineElapsed();
    g_app.dancePaused = true;
}

// Re-anchor danceStartTime so UpdateDancePlayback continues from the paused offset,
// honoring the current speed.
void ResumeDancePlayback() {
    if (!g_app.dancePlaying || !g_app.dancePaused) {
        return;
    }
    SetDanceStartForElapsed(ClampDanceElapsed(g_app.dancePausedElapsed));
    g_app.dancePaused = false;
}

// Stop any dance playback and rebuild the T-pose at the dance's start anchor. The
// dance animation can drift the HMD away from where playback began, so the HMD XZ
// is snapped back to danceRootX/Z before the reset to keep the T-pose at the right
// world position. Callers stream the resulting frame themselves.
void StopDanceToTPose() {
    if (g_app.dancePlaying) {
        g_app.frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.x = g_app.danceRootX;
        g_app.frame.devices[DeviceSlot(DeviceIndex::Hmd)].position.z = g_app.danceRootZ;
    }
    g_app.dancePlaying = false;
    g_app.dancePaused = false;
    g_app.dancePausedElapsed = 0.0f;
    g_app.frame = BuildResetTPose(g_app.frame);
}

void StopDancePlayback() {
    if (!g_app.dancePlaying) {
        return;
    }
    StopDanceToTPose();
    g_app.streamer.UpdateFrame(g_app.frame, En(Text::ResetReason), false);
}

// Apply a loaded .nya frame as the live pose. Device poses persist in g_app.frame
// (only mouse/reset/dance move them), while finger bends go through g_app.fingerBends
// so the per-frame ApplyFingerBend keeps them instead of overwriting with the wheel
// state. A restored pose takes over from any in-progress dance playback.
void RestorePose(const FrameState& pose) {
    g_app.dancePlaying = false;
    g_app.dancePaused = false;
    g_app.dancePausedElapsed = 0.0f;
    for (std::size_t d = 0; d < g_app.frame.devices.size(); ++d) {
        g_app.frame.devices[d].position = pose.devices[d].position;
        g_app.frame.devices[d].rotation = pose.devices[d].rotation;
        g_app.frame.devices[d].valid = true;
        g_app.frame.devices[d].connected = true;
    }
    for (std::size_t i = 0; i < 2; ++i) {
        if (pose.controllers[i].has_finger_bends) {
            g_app.fingerBends[i] = pose.controllers[i].finger_bends;
        }
    }
    g_app.streamer.UpdateFrame(FrameWithCurrentFingerBends(), "Pose restored", false);
}

// Pick up a finished Blender solve, retarget it, and report the outcome. Runs
// once per frame from the main loop.
void PollDanceExport() {
    if (!g_app.danceConverting || !g_app.danceFuture.valid()) {
        return;
    }
    if (g_app.danceFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    const MmdExportResult result = g_app.danceFuture.get();
    g_app.danceConverting = false;

    if (!result.ok) {
        g_app.danceStatus = result.message;
        return;
    }
    SolvedMotion motion;
    std::string error;
    if (!ParseSolvedMotion(result.solvedJson, motion, error)) {
        g_app.danceStatus = "Parse failed: " + error;
        return;
    }
    MmdRetargetParams params;
    params.targetHeightM = g_app.danceHeight;
    params.handReachScale = g_app.danceHandReach;
    g_app.danceMotion = BuildDanceMotion(motion, params);
    if (!g_app.danceMotion.valid) {
        g_app.danceStatus = "Retarget failed.";
        return;
    }
    g_app.dancePlaying = false;
    g_app.dancePaused = false;
    g_app.dancePausedElapsed = 0.0f;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Ready: %.1fs, %zu frames, fingers %s, scale %.2f",
                  g_app.danceMotion.duration,
                  g_app.danceMotion.frames.size(),
                  g_app.danceMotion.hasFingers ? "yes" : "no",
                  g_app.danceMotion.scale);
    g_app.danceStatus = buf;
}

// Drive one frame of MMD playback: sample the dance pose, anchor it in place,
// keep the live joystick/keyboard inputs (turning stays on the joystick), and
// re-apply the dance's finger bends on top.
void UpdateDancePlayback() {
    if (!g_app.dancePlaying || g_app.dancePaused || !g_app.danceMotion.valid) {
        return;  // paused playback holds the last streamed pose in place
    }
    float elapsed = DanceTimelineElapsed();
    if (!g_app.danceLoop && g_app.danceMotion.duration > 0.0f && elapsed >= g_app.danceMotion.duration) {
        elapsed = g_app.danceMotion.duration;  // hold the final pose at the end
    }
    g_app.dancePausedElapsed = elapsed;
    ApplyDanceFrameAt(elapsed);
}

// File-picker helpers for the dance dialog; fill the target buffer on success.
void BrowseInto(HWND hwnd, char* buffer, std::size_t size, Text label, const char* pattern) {
    const std::string picked = OpenFileDialog(hwnd, Tr(label), Tr(label), pattern);
    if (!picked.empty()) {
        std::snprintf(buffer, size, "%s", picked.c_str());
    }
}

// The MMD dance dialog: choose a VMD + model, verify (solve), and play. Blender
// does the FK/IK solve; the remap onto the hardcoded rig happens in-tool.
void RenderDanceDialog(HWND hwnd) {
    // OpenPopup and BeginPopupModal must use the exact same string: ImGui hashes
    // the "###id" suffix verbatim, so a localized label plus a fixed "###mmd_dance"
    // id keeps both calls in sync and survives a language switch.
    const std::string title = std::string(Tr(Text::DanceTitle)) + "###mmd_dance";
    if (g_app.danceDialogOpen) {
        ImGui::OpenPopup(title.c_str());
        g_app.danceDialogOpen = false;
        // Persisted advanced paths win over auto-detection. Empty fields mean the
        // user has not picked an override yet, so show the detected path as a
        // starting point.
        if (g_app.danceBlenderPath[0] == '\0') {
            CopyPreferenceString(g_app.danceBlenderPath, sizeof(g_app.danceBlenderPath), DetectBlenderExe());
        }
        if (g_app.danceMmdToolsPath[0] == '\0') {
            CopyPreferenceString(g_app.danceMmdToolsPath, sizeof(g_app.danceMmdToolsPath), DetectMmdToolsPath());
        }
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f, 0.0f), ImGuiCond_Appearing);
    // Passing a bool gives the modal a title-bar close (X) button; ImGui closes
    // the popup when it is clicked, so the dialog needs no explicit Close button.
    bool stayOpen = true;
    if (!ImGui::BeginPopupModal(title.c_str(), &stayOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::PushTextWrapPos(620.0f);
    ImGui::TextUnformatted(Tr(Text::DanceHelp));
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.34f, 1.0f), "%s", Tr(Text::DanceExperimental));
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    // Each path row uses a fixed-width label column so every input box is the same
    // length and left-aligned, with a Browse button pinned to the right edge.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float browseWidth = ImGui::CalcTextSize(Tr(Text::DanceBrowse)).x + style.FramePadding.x * 2.0f;
    float labelColumn = 0.0f;
    for (Text label : {Text::DanceVmd, Text::DanceModel, Text::DanceBlenderPath, Text::DanceMmdToolsPath}) {
        labelColumn = std::max(labelColumn, ImGui::CalcTextSize(Tr(label)).x);
    }
    auto pathRow = [&](const char* id, Text label, char* buffer, std::size_t size, auto&& onBrowse) {
        const float rowStart = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Tr(label));
        ImGui::SameLine();
        ImGui::SetCursorPosX(rowStart + labelColumn + style.ItemSpacing.x);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseWidth - style.ItemSpacing.x);
        ImGui::InputText((std::string("##") + id).c_str(), buffer, size);
        ImGui::SameLine();
        ImGui::PushID(id);
        if (ImGui::Button(Tr(Text::DanceBrowse), ImVec2(browseWidth, 0.0f))) {
            onBrowse();
        }
        ImGui::PopID();
    };

    pathRow("vmd", Text::DanceVmd, g_app.danceVmdPath, sizeof(g_app.danceVmdPath), [&] {
        BrowseInto(hwnd, g_app.danceVmdPath, sizeof(g_app.danceVmdPath), Text::DanceVmd, "*.vmd");
    });
    pathRow("model", Text::DanceModel, g_app.danceModelPath, sizeof(g_app.danceModelPath), [&] {
        BrowseInto(hwnd, g_app.danceModelPath, sizeof(g_app.danceModelPath), Text::DanceModel, "*.pmx;*.pmd");
    });

    // Analyze runs the Blender solve and lives right under the inputs it consumes.
    ImGui::BeginDisabled(g_app.danceConverting);
    if (ImGui::Button(Tr(Text::DanceAnalyze), ImVec2(-1.0f, 0.0f))) {
        StartDanceExport();
    }
    ImGui::EndDisabled();

    // Advanced: only needed if Blender / MMD Tools were not auto-detected. The
    // fields are pre-filled with the detected paths when the dialog opens.
    if (ImGui::CollapsingHeader(Tr(Text::DanceAdvanced))) {
        pathRow("blender", Text::DanceBlenderPath, g_app.danceBlenderPath, sizeof(g_app.danceBlenderPath), [&] {
            BrowseInto(hwnd, g_app.danceBlenderPath, sizeof(g_app.danceBlenderPath), Text::DanceBlenderPath, "*.exe");
        });
        pathRow("mmdtools", Text::DanceMmdToolsPath, g_app.danceMmdToolsPath, sizeof(g_app.danceMmdToolsPath), [&] {
            const std::string picked = OpenFolderDialog(hwnd, Tr(Text::DanceMmdToolsPath));
            if (!picked.empty()) {
                std::snprintf(g_app.danceMmdToolsPath, sizeof(g_app.danceMmdToolsPath), "%s", picked.c_str());
            }
        });
    }

    ImGui::Separator();
    const float duration = g_app.danceMotion.valid ? g_app.danceMotion.duration : 0.0f;
    float timeline = DanceTimelineElapsed();
    ImGui::Text("%s: %.2fs / %.2fs", Tr(Text::DanceTimeline), timeline, duration);
    ImGui::BeginDisabled(!g_app.danceMotion.valid || duration <= 0.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##dance_timeline", &timeline, 0.0f, duration, "")) {
        SeekDancePlayback(timeline);
    }
    ImGui::EndDisabled();

    // Play stays disabled until a solve (or a loaded clip) is ready and (re)starts
    // from the top; Pause/Resume freezes and continues in place; Stop returns to the
    // T-pose; the Loop checkbox fills the last cell of the row.
    const float quadWidth = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 3.0f) / 4.0f;
    ImGui::BeginDisabled(g_app.danceConverting || !g_app.danceMotion.valid);
    if (ImGui::Button(Tr(Text::DancePlay), ImVec2(quadWidth, 0.0f))) {
        StartDancePlayback();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_app.dancePlaying);
    if (ImGui::Button(g_app.dancePaused ? Tr(Text::DanceResume) : Tr(Text::DancePause),
                      ImVec2(quadWidth, 0.0f))) {
        if (g_app.dancePaused) {
            ResumeDancePlayback();
        } else {
            PauseDancePlayback();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_app.dancePlaying);
    if (ImGui::Button(Tr(Text::DanceStop), ImVec2(quadWidth, 0.0f))) {
        StopDancePlayback();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox(Tr(Text::DanceLoop), &g_app.danceLoop);

    // Save the analyzed motion as a .nya clip, or load one to play directly. A
    // loaded clip skips both Blender and the remap, so Play is ready immediately.
    const float halfWidth = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.0f;
    ImGui::BeginDisabled(!g_app.danceMotion.valid);
    if (ImGui::Button(Tr(Text::DanceSaveNya), ImVec2(halfWidth, 0.0f))) {
        const std::string path = SaveFileDialog(hwnd, Tr(Text::DanceSaveNya), "AnyaDance (*.nya)", "*.nya", "nya");
        if (!path.empty()) {
            NyaClip clip = MakeAnimationClip(g_app.danceMotion, g_app.danceFps, g_app.danceModelPath);
            clip.loop = g_app.danceLoop;
            g_app.danceStatus = WriteFileUtf8(path, SerializeNya(clip))
                                    ? ("Saved " + path)
                                    : std::string("Could not write the .nya file.");
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(Tr(Text::DanceLoadNya), ImVec2(halfWidth, 0.0f))) {
        const std::string path = OpenFileDialog(hwnd, Tr(Text::DanceLoadNya), "AnyaDance (*.nya)", "*.nya");
        if (!path.empty()) {
            NyaClip clip;
            std::string error;
            if (ParseNya(ReadFileUtf8(path), clip, error)) {
                g_app.danceMotion = clip.motion;
                g_app.danceLoop = clip.loop;
                g_app.dancePlaying = false;
                g_app.dancePaused = false;
                g_app.dancePausedElapsed = 0.0f;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "Loaded %zu frames, %.1fs, fingers %s. Press Play.",
                              clip.motion.frames.size(), clip.motion.duration,
                              clip.motion.hasFingers ? "yes" : "no");
                g_app.danceStatus = buf;
            } else {
                g_app.danceStatus = "Load failed: " + error;
            }
        }
    }

    if (!g_app.danceStatus.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(620.0f);
        ImGui::TextWrapped("%s", g_app.danceStatus.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndPopup();
}

// Build the whole single-page UI for one frame: Reset, driver controls and
// status, the body + log panels, and the language/help footer.
void RenderUi(HWND hwnd) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("AnyaDance", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    RenderUiModeControls(hwnd, true);
    ImGui::Separator();

    if (ImGui::Button(Tr(Text::Reset), ImVec2(-1, 44))) {
        StopDanceToTPose();  // a manual reset also stops any MMD playback
        g_app.keyboard.Neutralize();
        g_app.fingerBends[0] = FingerBends{};
        g_app.fingerBends[1] = FingerBends{};
        g_app.streamer.UpdateFrame(g_app.frame, En(Text::ResetReason), false);
    }

    const auto recordStatus = [](const DriverActionResult& result) {
        g_app.driverStatusSet = true;
        g_app.driverStatus = result.status;
        g_app.driverStatusDetail = result.detail;
    };

    bool restartConfirmRequested = false;
    if (ImGui::BeginTable("main_controls", 2, ImGuiTableFlags_SizingStretchSame)) {
        // Save the current pose (device poses + finger bends) to a .nya file, or
        // load one back. A pose is a one-frame clip, so it shares the format with
        // dances.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(Tr(Text::PoseSave), ImVec2(-1.0f, 0.0f))) {
            const std::string path = SaveFileDialog(hwnd, Tr(Text::PoseSave), "AnyaDance (*.nya)", "*.nya", "nya");
            if (!path.empty() && !WriteFileUtf8(path, SerializeNya(MakePoseClip(FrameWithCurrentFingerBends())))) {
                MessageBoxA(hwnd, "Could not write the .nya file.", "AnyaDance", MB_OK | MB_ICONWARNING);
            }
        }
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(Tr(Text::PoseLoad), ImVec2(-1.0f, 0.0f))) {
            const std::string path = OpenFileDialog(hwnd, Tr(Text::PoseLoad), "AnyaDance (*.nya)", "*.nya");
            if (!path.empty()) {
                NyaClip clip;
                std::string error;
                if (ParseNya(ReadFileUtf8(path), clip, error) && !clip.motion.frames.empty()) {
                    RestorePose(clip.motion.frames.front());
                } else {
                    MessageBoxA(hwnd, ("Load failed: " + error).c_str(), "AnyaDance", MB_OK | MB_ICONWARNING);
                }
            }
        }

        // Driver/system controls stay in one compact row in the left column; the
        // MMD dance entry point starts the right column below Load Pose.
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const float systemButtonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
        if (ImGui::Button(Tr(Text::RegisterDriver), ImVec2(systemButtonWidth, 0.0f))) {
            recordStatus(RegisterDriver());
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(Text::UnregisterDriver), ImVec2(systemButtonWidth, 0.0f))) {
            recordStatus(UnregisterDriver());
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(Text::RestartSteamVr), ImVec2(systemButtonWidth, 0.0f))) {
            restartConfirmRequested = true;
        }
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(Tr(Text::DanceOpen), ImVec2(-1.0f, 0.0f))) {
            g_app.danceDialogOpen = true;
        }
        ImGui::EndTable();
    }

    if (restartConfirmRequested) {
        ImGui::OpenPopup("restart_confirm");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("restart_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(440.0f);
        ImGui::TextUnformatted(Tr(Text::RestartConfirmBody));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(Tr(Text::RestartSteamVr), ImVec2(160.0f, 0.0f))) {
            recordStatus(RestartSteamVR());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(Text::Cancel), ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_app.driverStatusSet) {
        std::string status = Tr(DriverStatusText(g_app.driverStatus));
        if (!g_app.driverStatusDetail.empty()) {
            status += " (" + g_app.driverStatusDetail + ")";
        }
        ImGui::TextWrapped("%s", status.c_str());
    } else {
        ImGui::TextWrapped("%s", Tr(Text::DriverStatusReady));
    }

    ImGui::Separator();
    const float footerHeight = 74.0f;
    ImGui::BeginChild("main", ImVec2(0, -footerHeight), false);
    RenderBodyPanel(hwnd);
    ImGui::SameLine();
    RenderLogPanel();
    ImGui::EndChild();

    ImGui::Separator();
    const ImVec2 footerMin = ImGui::GetCursorScreenPos();
    const ImVec2 footerSize = ImGui::GetContentRegionAvail();
    const float footerTextMaxX = DrawFooterBanner(footerMin, footerSize);
    ImGui::PushTextWrapPos(footerTextMaxX);
    ImGui::Text("%s: ", Tr(Text::LanguageLabel));
    for (std::size_t i = 0; i < kLanguageCount; ++i) {
        const Language language = static_cast<Language>(i);
        ImGui::SameLine();
        if (ImGui::RadioButton(GetLanguageInfo(language).displayName, CurrentLanguage() == language)) {
            SetCurrentLanguage(language);
            g_app.streamer.SetLocalizedResults(En(Text::Sent), En(Text::Failed), En(Text::SocketErrorReason), En(Text::ReleaseReason));
        }
    }
    ImGui::TextUnformatted(Tr(Text::KeyLine1));
    ImGui::TextUnformatted(Tr(Text::KeyLine2));
    ImGui::PopTextWrapPos();

    RenderDanceDialog(hwnd);

    ImGui::End();
}

void RenderMiniUi(HWND hwnd) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("AnyaDance", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    RenderUiModeControls(hwnd, true);
    ImGui::Separator();
    RenderBodyPanel(hwnd, true);

    ImGui::End();
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // The mouse wheel is handled inside the frame (see RenderBodyPanel) so that
    // ImGui window z-order decides whether it bends fingers or scrolls the log.
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }
    switch (msg) {
    case WM_GETMINMAXINFO: {
        MINMAXINFO* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
        minMax->ptMinTrackSize.x = MinWindowWidth();
        minMax->ptMinTrackSize.y = MinWindowHeight();
        return 0;
    }
    case WM_SIZE:
        if (g_device != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        HandleFocusLost();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

const ImWchar* BuildUiGlyphRanges(ImGuiIO& io) {
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    for (std::size_t l = 0; l < kLanguageCount; ++l) {
        const Language language = static_cast<Language>(l);
        builder.AddText(GetLanguageInfo(language).displayName);
        for (std::size_t t = 0; t < kTextCount; ++t) {
            builder.AddText(Tr(static_cast<Text>(t), language));
        }
    }
    builder.BuildRanges(&g_uiGlyphRanges);
    return g_uiGlyphRanges.Data;
}
ImFont* LoadUiFont(ImGuiIO& io) {
    const ImWchar* ranges = BuildUiGlyphRanges(io);
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.RasterizerMultiply = 1.15f;
    const char* fontCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\NotoSansCJK-Regular.ttc",
        "C:\\Windows\\Fonts\\NotoSansSC-Regular.otf",
    };
    for (const char* path : fontCandidates) {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, 16.0f, &config, ranges)) {
            return font;
        }
    }
    return io.Fonts->AddFontDefault();
}

enum class DisclaimerAction { None, Accept, Quit };

DisclaimerAction RenderDisclaimerGate() {
    DisclaimerAction action = DisclaimerAction::None;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(660.0f, 0.0f));
    ImGui::Begin("disclaimer_gate", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted(Tr(Text::LanguageLabel));
    for (std::size_t i = 0; i < kLanguageCount; ++i) {
        const Language language = static_cast<Language>(i);
        ImGui::SameLine();
        if (ImGui::RadioButton(GetLanguageInfo(language).displayName, CurrentLanguage() == language)) {
            SetCurrentLanguage(language);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted(Tr(Text::DisclaimerTitle));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(Tr(Text::DisclaimerBody));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button(Tr(Text::DisclaimerAccept), ImVec2(280.0f, 36.0f))) {
        action = DisclaimerAction::Accept;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(Text::DisclaimerQuit), ImVec2(140.0f, 36.0f))) {
        action = DisclaimerAction::Quit;
    }
    ImGui::End();
    return action;
}
} // namespace

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    ScopedHandle instanceMutex(CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName));
    if (!instanceMutex.handle) {
        const DWORD err = GetLastError();
        wchar_t msg[256]{};
        swprintf_s(msg, L"CreateMutexW failed. GetLastError = %lu", err);
        MessageBoxW(nullptr, msg, kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClassName, kWindowTitle)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0L;
    wc.cbWndExtra = 0L;
    wc.hInstance = hInstance;
    wc.hIcon = appIcon;
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = kWindowClassName;
    wc.hIconSm = appIcon;

    ATOM classAtom = RegisterClassExW(&wc);
    if (classAtom == 0) {
        const DWORD err = GetLastError();
        wchar_t msg[256]{};
        swprintf_s(msg, L"RegisterClassExW failed. GetLastError = %lu", err);
        MessageBoxW(nullptr, msg, kWindowTitle, MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comInit)) {
            CoUninitialize();
        }
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        kDefaultWindowWidth,
        kDefaultWindowHeight,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        const DWORD err = GetLastError();
        wchar_t msg[256]{};
        swprintf_s(msg, L"CreateWindowExW failed. GetLastError = %lu", err);
        MessageBoxW(nullptr, msg, kWindowTitle, MB_OK | MB_ICONERROR);
        UnregisterClassW(kWindowClassName, hInstance);
        if (SUCCEEDED(comInit)) {
            CoUninitialize();
        }
        return 1;
    }

    SetWindowTextW(hwnd, kWindowTitle);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(kWindowClassName, hInstance);
        if (SUCCEEDED(comInit)) {
            CoUninitialize();
        }
        return 1;
    }

    LoadPngResourceTexture(
        hInstance,
        IDR_ANYA_PNG,
        &g_bodyBackgroundTexture,
        &g_bodyBackgroundWidth,
        &g_bodyBackgroundHeight
    );
    LoadPngResourceTexture(
        hInstance,
        IDR_ANYA_BANNER_PNG,
        &g_footerBannerTexture,
        &g_footerBannerWidth,
        &g_footerBannerHeight
    );

    LoadPreferences(hwnd);
    ShowWindow(hwnd, nCmdShow == 0 ? SW_SHOWDEFAULT : nCmdShow);
    UpdateWindow(hwnd);
    ApplyAlwaysOnTop(hwnd, g_app.alwaysOnTop);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    g_uiFont = LoadUiFont(io);
    io.FontDefault = g_uiFont;
    g_monoFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 13.0f);

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_deviceContext);

    g_app.streamer.SetLocalizedResults(En(Text::Sent), En(Text::Failed), En(Text::SocketErrorReason), En(Text::ReleaseReason));
    // Do not stream until the disclaimer has been accepted.
    if (g_app.disclaimerAccepted) {
        g_app.streamer.Start(g_app.frame);
    }

    bool done = false;
    while (!done) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        if (g_app.disclaimerAccepted) {
            HandleKeyboard(hwnd);
            UpdateCapture(hwnd);
            PollDanceExport();
            UpdateDancePlayback();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_app.disclaimerAccepted) {
            if (g_app.uiMode == UiMode::Mini) {
                RenderMiniUi(hwnd);
            } else {
                RenderUi(hwnd);
            }
        } else {
            switch (RenderDisclaimerGate()) {
            case DisclaimerAction::Accept:
                g_app.disclaimerAccepted = true;
                SavePreferences(hwnd);
                g_app.streamer.Start(g_app.frame);
                break;
            case DisclaimerAction::Quit:
                done = true;
                break;
            case DisclaimerAction::None:
                break;
            }
        }

        ImGui::Render();

        const float clearColor[4] = {0.08f, 0.09f, 0.10f, 1.00f};
        g_deviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_deviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);

        ApplyPendingUiMode(hwnd);
    }

    ReleaseMouseCapture();
    SavePreferences(hwnd);
    g_app.streamer.Stop();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClassName, hInstance);

    if (SUCCEEDED(comInit)) {
        CoUninitialize();
    }

    return 0;
}
