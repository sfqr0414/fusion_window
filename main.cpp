#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <uxtheme.h> 
#include <avrt.h> 
#include <iostream>
#include <thread>
#include <atomic>
#include <variant>
#include <vector>
#include <memory>
#include <chrono>
#include <memory_resource>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl.h>
#include <uianimation.h> 

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "Synchronization.lib")
// #pragma comment(lib, "UIAnimation.lib") 

using namespace Microsoft::WRL;

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

// =========================================================
// 【架构开关】：ImGui 桥接
// =========================================================
// #define USE_IMGUI

#ifdef USE_IMGUI
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

// =========================================================
// 1. 动态 DPI 标题栏高度与 DWM 热区计算 (致敬你的代码)
// =========================================================
float g_dpiScale = 1.0f;
inline int S(int val) { return (int)(val * g_dpiScale); }

inline auto compute_standard_caption_height_for_window(HWND window_handle) {
    auto const accounting_for_borders = 2;
    auto dpi = GetDpiForWindow(window_handle);
    RECT rcFrame = { 0 };
    AdjustWindowRectExForDpi(&rcFrame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    return -rcFrame.top - accounting_for_borders;
}
std::atomic<int> g_CaptionHeight{ 32 };

inline auto compute_sector_of_window(HWND window_handle, WPARAM, LPARAM lparam, int caption_height) -> LRESULT {
    RECT window_rectangle;
    GetWindowRect(window_handle, &window_rectangle);
    auto offset = GetSystemMetricsForDpi(SM_CXFRAME, GetDpiForWindow(window_handle)) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(window_handle));
    POINT cursor_position{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };

    if (cursor_position.y < window_rectangle.top + offset && cursor_position.x < window_rectangle.left + offset) return HTTOPLEFT;
    if (cursor_position.y < window_rectangle.top + offset && cursor_position.x > window_rectangle.right - offset) return HTTOPRIGHT;
    if (cursor_position.y > window_rectangle.bottom - offset && cursor_position.x > window_rectangle.right - offset) return HTBOTTOMRIGHT;
    if (cursor_position.y > window_rectangle.bottom - offset && cursor_position.x < window_rectangle.left + offset) return HTBOTTOMLEFT;

    if (cursor_position.x > window_rectangle.left && cursor_position.x < window_rectangle.right) {
        if (cursor_position.y < window_rectangle.top + offset) return HTTOP;
        else if (cursor_position.y > window_rectangle.bottom - offset) return HTBOTTOM;
    }
    if (cursor_position.y > window_rectangle.top && cursor_position.y < window_rectangle.bottom) {
        if (cursor_position.x < window_rectangle.left + offset) return HTLEFT;
        else if (cursor_position.x > window_rectangle.right - offset) return HTRIGHT;
    }
    if (cursor_position.x > window_rectangle.left && cursor_position.x < window_rectangle.right) {
        if (cursor_position.y < window_rectangle.top + caption_height) return HTCAPTION;
    }
    return HTNOWHERE;
}

// =========================================================
// 【修复 C3861】：提前声明菜单相关函数和数据
// =========================================================
struct MenuItemInfo { const wchar_t* text; float width; };
MenuItemInfo g_MenuItems[] = { { L"文件(F)", 0.0f }, { L"编辑(E)", 0.0f }, { L"视图(V)", 0.0f }, { L"帮助(H)", 0.0f } };
const int g_MenuCount = 4;
const float g_MenuPadding = 20.0f;
const float g_MenuStartX = 210.0f;

float GetTotalMenuWidth() {
    float total = 0.0f;
    for (int i = 0; i < g_MenuCount; ++i) total += g_MenuItems[i].width;
    return total;
}

void CalculateMenuWidths() {
    ComPtr<IDWriteFactory> dwriteFactory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf())))) return;
    ComPtr<IDWriteTextFormat> menuFormat;
    // 菜单字体 14px，标准样式
    dwriteFactory->CreateTextFormat(L"Microsoft YaHei UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-cn", &menuFormat);
    for (int i = 0; i < g_MenuCount; ++i) {
        ComPtr<IDWriteTextLayout> textLayout;
        dwriteFactory->CreateTextLayout(g_MenuItems[i].text, static_cast<UINT32>(wcslen(g_MenuItems[i].text)), menuFormat.Get(), 10000.0f, 10000.0f, &textLayout);
        DWRITE_TEXT_METRICS metrics; textLayout->GetMetrics(&metrics);
        g_MenuItems[i].width = metrics.width + g_MenuPadding;
    }
}

int GetMenuIndexFromLogicalX(float logicalX) {
    float currentX = g_MenuStartX;
    for (int i = 0; i < g_MenuCount; ++i) {
        if (logicalX >= currentX && logicalX < currentX + g_MenuItems[i].width) return i;
        currentX += g_MenuItems[i].width;
    }
    return -1;
}

HFONT g_hFont = NULL;
void UpdateGlobalFont() {
    if (g_hFont) DeleteObject(g_hFont);
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)(96 * g_dpiScale));
    ncm.lfMessageFont.lfHeight = -S(14); // 字体调大，更加和谐
    ncm.lfMessageFont.lfWeight = FW_NORMAL;
    ncm.lfMessageFont.lfItalic = FALSE;
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
}

// =========================================================
// 2. 无锁变体指令集
// =========================================================
struct CmdResize { int width; int height; };
struct CmdChangeColor { float r; float g; float b; };
struct CmdSetGrid { bool show; };
struct CmdSetAimRadius { float radius; };
struct CmdSetAimStyle { int style; };
struct CmdAddHitMark { float x; float y; };
struct CmdResetCanvas {};
struct CmdKeyDown { WPARAM key; };
struct CmdWin32Msg { HWND hwnd; UINT msg; WPARAM wParam; LPARAM lParam; };
struct CmdExit {};
using RenderCommand = std::variant<CmdResize, CmdChangeColor, CmdSetGrid, CmdSetAimRadius, CmdSetAimStyle, CmdAddHitMark, CmdResetCanvas, CmdKeyDown, CmdWin32Msg, CmdExit>;

// =========================================================
// 3. PMR 双向回收无锁队列
// =========================================================
template<typename T>
class PmrRecyclingSPSCQueue {
private:
    struct Node { T data; std::atomic<Node*> next{ nullptr }; };
    alignas(64) std::atomic<Node*> head; alignas(64) std::atomic<Node*> tail;
    alignas(64) std::atomic<Node*> recycle_head; alignas(64) std::atomic<Node*> recycle_tail;
    std::pmr::unsynchronized_pool_resource pool;
    static std::pmr::pool_options make_pmr_opts() { std::pmr::pool_options opts{}; opts.max_blocks_per_chunk = 32; opts.largest_required_pool_block = sizeof(Node); return opts; }
    Node* allocate_node() { void* mem = pool.allocate(sizeof(Node), alignof(Node)); return new (mem) Node{}; }
    void recycle_push(Node* node) {
        node->next.store(nullptr, std::memory_order_relaxed);
        Node* curr_rtail = recycle_tail.load(std::memory_order_relaxed);
        curr_rtail->next.store(node, std::memory_order_release);
        recycle_tail.store(node, std::memory_order_release);
    }
    Node* recycle_pop() {
        Node* curr_rhead = recycle_head.load(std::memory_order_relaxed);
        Node* next_rnode = curr_rhead->next.load(std::memory_order_acquire);
        if (!next_rnode) return nullptr;
        recycle_head.store(next_rnode, std::memory_order_release); return curr_rhead;
    }
public:
    PmrRecyclingSPSCQueue() : pool(make_pmr_opts()) {
        Node* d1 = allocate_node(); head.store(d1); tail.store(d1);
        Node* d2 = allocate_node(); recycle_head.store(d2); recycle_tail.store(d2);
    }
    ~PmrRecyclingSPSCQueue() {
        auto clear_queue = [](std::atomic<Node*>& q_head) { while (Node* curr = q_head.load()) { q_head.store(curr->next.load()); curr->~Node(); } };
        clear_queue(head); clear_queue(recycle_head);
    }
    void push(T item) {
        Node* new_node = recycle_pop();
        if (!new_node) new_node = allocate_node();
        new_node->data = std::move(item); new_node->next.store(nullptr, std::memory_order_relaxed);
        Node* current_tail = tail.load(std::memory_order_relaxed);
        current_tail->next.store(new_node, std::memory_order_release);
        tail.store(new_node, std::memory_order_release);
    }
    bool pop(T& item) {
        Node* current_head = head.load(std::memory_order_relaxed);
        Node* next_node = current_head->next.load(std::memory_order_acquire);
        if (!next_node) return false;
        item = std::move(next_node->data);
        head.store(next_node, std::memory_order_release);
        recycle_push(current_head); return true;
    }
    bool is_empty() const { return head.load(std::memory_order_relaxed) == tail.load(std::memory_order_acquire); }
};
PmrRecyclingSPSCQueue<RenderCommand> g_CommandQueue;

// =========================================================
// 4. 混合自旋 Futex (完美保留你的要求)
// =========================================================
class FutexEvent {
    volatile LONG m_signal = 0;
public:
    void Notify() { InterlockedIncrement(&m_signal); WakeByAddressSingle((PVOID)&m_signal); }
    // 【你要求的 Wait】
    void Wait(LONG expected) { WaitOnAddress((PVOID)&m_signal, &expected, sizeof(LONG), INFINITE); }
    bool HybridWait(LONG expected, int spin_us = 1500, DWORD timeout_ms = INFINITE) {
        auto start = std::chrono::steady_clock::now();
        while (m_signal == expected) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::microseconds>(now - start).count() < spin_us) { _mm_pause(); }
            else { return WaitOnAddress((PVOID)&m_signal, &expected, sizeof(LONG), timeout_ms); }
        }
        return true;
    }
    LONG Capture() const { return m_signal; }
};

FutexEvent g_RenderEvent, g_GpuInitDoneEvent;
std::atomic<bool> g_isRunning(true);
HWND g_hMainWindow = NULL, g_hCanvas = NULL, g_hPanel = NULL;
DWORD g_UIThreadId = 0;
std::atomic<bool> g_bMouseInCanvas(false);
std::atomic<uint64_t> g_LastMousePosPacked(0);
std::atomic<bool> g_ImGuiWantCaptureMouse(false);
std::atomic<bool> g_IsTitleMenuVisible(false);

// =========================================================
// 5. UIAnimation 硬件定时器引擎
// =========================================================
ComPtr<IUIAnimationManager> g_AnimManager;
ComPtr<IUIAnimationTimer> g_AnimTimer;
ComPtr<IUIAnimationTransitionLibrary> g_AnimLibrary;
ComPtr<IUIAnimationVariable> g_AnimProgressVar;

void StartProgressPingPongAnimation() {
    if (!g_AnimManager || !g_AnimProgressVar) return;
    double currentValue = 0; g_AnimProgressVar->GetValue(&currentValue);
    ComPtr<IUIAnimationStoryboard> storyboard; g_AnimManager->CreateStoryboard(&storyboard);
    double targetValue = (currentValue < 50.0) ? 100.0 : 0.0;
    ComPtr<IUIAnimationTransition> transition;
    g_AnimLibrary->CreateAccelerateDecelerateTransition(2.0, targetValue, 0.3, 0.3, &transition);
    storyboard->AddTransition(g_AnimProgressVar.Get(), transition.Get());
    UI_ANIMATION_SECONDS timeNow; g_AnimTimer->GetTime(&timeNow);
    storyboard->Schedule(timeNow);
}

#define WM_UPDATE_PROGRESS (WM_USER + 100)
class CAnimationEventHandler : public IUIAnimationTimerEventHandler {
    ULONG m_refCount = 1;
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IUIAnimationTimerEventHandler)) { *ppv = static_cast<IUIAnimationTimerEventHandler*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    IFACEMETHODIMP_(ULONG) Release() override { ULONG res = InterlockedDecrement(&m_refCount); if (res == 0) delete this; return res; }
    IFACEMETHODIMP OnPreUpdate() override { return S_OK; }
    IFACEMETHODIMP OnPostUpdate() override {
        if (!g_AnimProgressVar || !g_hPanel) return S_OK;
        double val = 0; g_AnimProgressVar->GetValue(&val);
        PostMessage(g_hPanel, WM_UPDATE_PROGRESS, (WPARAM)val, 0);
        UI_ANIMATION_MANAGER_STATUS status; g_AnimManager->GetStatus(&status);
        if (status == UI_ANIMATION_MANAGER_IDLE) StartProgressPingPongAnimation();
        return S_OK;
    }
    IFACEMETHODIMP OnRenderingTooSlow(UINT32 framesPerSecond) override { return S_OK; }
};

void InitUIAnimation() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(CLSID_UIAnimationManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_AnimManager));
    CoCreateInstance(CLSID_UIAnimationTimer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_AnimTimer));
    CoCreateInstance(CLSID_UIAnimationTransitionLibrary, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_AnimLibrary));
    g_AnimManager->CreateAnimationVariable(0.0, &g_AnimProgressVar);
    ComPtr<IUIAnimationTimerUpdateHandler> updateHandler; g_AnimManager.As(&updateHandler);
    g_AnimTimer->SetTimerUpdateHandler(updateHandler.Get(), UI_ANIMATION_IDLE_BEHAVIOR_DISABLE);
    ComPtr<CAnimationEventHandler> pEventHandler = new CAnimationEventHandler();
    g_AnimTimer->SetTimerEventHandler(pEventHandler.Get());
}

DWORD_PTR GetPCoreMask() {
    DWORD length = 0; GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return 0;
    std::vector<BYTE> buffer(length);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data(), &length)) return 0;
    BYTE maxEfficiency = 0;
    for (BYTE* p = buffer.data(); p < buffer.data() + length; p += ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p)->Size) {
        auto info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
        if (info->Relationship == RelationProcessorCore) maxEfficiency = max(maxEfficiency, info->Processor.EfficiencyClass);
    }
    DWORD_PTR pCoreMask = 0;
    for (BYTE* p = buffer.data(); p < buffer.data() + length; p += ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p)->Size) {
        auto info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
        if (info->Relationship == RelationProcessorCore && info->Processor.EfficiencyClass == maxEfficiency) {
            pCoreMask |= info->Processor.GroupMask[0].Mask;
        }
    }
    return pCoreMask & ~(pCoreMask - 1);
}

// =========================================================
// 6. ImGui Pimpl 桥接层 (你要求原样保留的完整版)
// =========================================================
class ImGuiBridge {
public:
    virtual ~ImGuiBridge() = default;
    virtual void Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context) = 0;
    virtual void ResizeOffscreenTarget(ID3D11Device* device, ID2D1DeviceContext* d2dContext, int width, int height) = 0;
    virtual void HandleWin32Message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) = 0;
    virtual void RenderFrame(ID3D11DeviceContext* context, float menuAlpha, bool& outShowGrid) = 0;
    virtual ID2D1Bitmap1* GetD2DTexture() = 0;
    virtual void Shutdown() = 0;
};

#ifdef USE_IMGUI
class ImGuiBridgeImpl : public ImGuiBridge {
private:
    ComPtr<ID3D11Texture2D> imguiTex;
    ComPtr<ID3D11RenderTargetView> imguiRTV;
    ComPtr<ID2D1Bitmap1> imguiD2DBitmap;
public:
    void Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context) override {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = NULL;
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX11_Init(device, context);
    }
    void ResizeOffscreenTarget(ID3D11Device* device, ID2D1DeviceContext* d2dContext, int width, int height) override {
        imguiTex.Reset(); imguiRTV.Reset(); imguiD2DBitmap.Reset();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        device->CreateTexture2D(&desc, nullptr, &imguiTex);
        device->CreateRenderTargetView(imguiTex.Get(), nullptr, &imguiRTV);
        ComPtr<IDXGISurface> imguiSurface;
        imguiTex.As(&imguiSurface);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_STRAIGHT));
        d2dContext->CreateBitmapFromDxgiSurface(imguiSurface.Get(), &bp, &imguiD2DBitmap);
    }
    void HandleWin32Message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    }

    // 【你原原本本要求贴回的代码！】
    void RenderFrame(ID3D11DeviceContext* context, float menuAlpha, bool& outShowGrid) override {
        const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
        context->OMSetRenderTargets(1, imguiRTV.GetAddressOf(), nullptr); context->ClearRenderTargetView(imguiRTV.Get(), clearColor);
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        g_ImGuiWantCaptureMouse.store(ImGui::GetIO().WantCaptureMouse, std::memory_order_relaxed);
        if (menuAlpha > 0.01f) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, menuAlpha);
            if (ImGui::BeginMainMenuBar()) {
                if (ImGui::BeginMenu("File")) { if (ImGui::MenuItem("Exit (ImGui)")) g_CommandQueue.push(CmdExit{}); ImGui::EndMenu(); }
                if (ImGui::BeginMenu("Settings")) { ImGui::MenuItem("Grid Enabled", NULL, &outShowGrid); ImGui::EndMenu(); }
                ImGui::EndMainMenuBar();
            }
            ImGui::PopStyleVar();
        }
        ImGui::Render(); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    ID2D1Bitmap1* GetD2DTexture() override { return imguiD2DBitmap.Get(); }
    void Shutdown() override { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
};
#else
class ImGuiBridgeImpl : public ImGuiBridge {
public:
    void Init(HWND, ID3D11Device*, ID3D11DeviceContext*) override { g_ImGuiWantCaptureMouse.store(false, std::memory_order_relaxed); }
    void ResizeOffscreenTarget(ID3D11Device*, ID2D1DeviceContext*, int, int) override {}
    void HandleWin32Message(HWND, UINT, WPARAM, LPARAM) override {}
    void RenderFrame(ID3D11DeviceContext*, float, bool&) override {}
    ID2D1Bitmap1* GetD2DTexture() override { return nullptr; }
    void Shutdown() override {}
};
#endif

// =========================================================
// 8. GPU 渲染线程
// =========================================================
void RenderThreadFunc() {
    DWORD renderThreadId = GetCurrentThreadId();
    if (!AttachThreadInput(renderThreadId, g_UIThreadId, TRUE)) return;

    DWORD_PTR pCoreMask = GetPCoreMask();
    if (pCoreMask) SetThreadAffinityMask(GetCurrentThread(), pCoreMask);

    // 【你要求的补回】：MMCSS 联想护盾
    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (hAvrt) AvSetMmThreadPriority(hAvrt, AVRT_PRIORITY_CRITICAL);

    ComPtr<ID3D11Device> d3dDevice; ComPtr<ID3D11DeviceContext> d3dContext;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext);
    ComPtr<IDXGIDevice1> dxgiDevice; d3dDevice.As(&dxgiDevice); dxgiDevice->SetMaximumFrameLatency(1);

    ComPtr<ID2D1Factory1> d2dFactory; D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &d2dFactory);
    ComPtr<ID2D1Device> d2dDevice; d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
    ComPtr<ID2D1DeviceContext> d2dContext; d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext);

    ComPtr<IDWriteFactory> dwriteFactory;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));

    // 【完美和谐】：标题 16px，菜单 14px，取消加粗倾斜
    ComPtr<IDWriteTextFormat> titleFormat; dwriteFactory->CreateTextFormat(L"Microsoft YaHei UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-cn", &titleFormat);
    ComPtr<IDWriteTextFormat> menuFormat;  dwriteFactory->CreateTextFormat(L"Microsoft YaHei UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-cn", &menuFormat);

    ComPtr<IDXGIAdapter> dxgiAdapter; dxgiDevice->GetAdapter(&dxgiAdapter);
    ComPtr<IDXGIFactory2> dxgiFactory; dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), &dxgiFactory);

    RECT rc; GetClientRect(g_hCanvas, &rc);
    int currentWidth = max(rc.right - rc.left, 1);
    int currentHeight = max(rc.bottom - rc.top, 1);

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = { 0 };
    swapChainDesc.Width = currentWidth; swapChainDesc.Height = currentHeight;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; swapChainDesc.BufferCount = 2;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    ComPtr<IDXGISwapChain1> swapChain;
    dxgiFactory->CreateSwapChainForComposition(d3dDevice.Get(), &swapChainDesc, nullptr, &swapChain);

    ComPtr<IDCompositionDevice> dcompDevice; DCompositionCreateDevice(dxgiDevice.Get(), __uuidof(IDCompositionDevice), &dcompDevice);
    ComPtr<IDCompositionTarget> dcompTarget; dcompDevice->CreateTargetForHwnd(g_hCanvas, TRUE, &dcompTarget);

    ComPtr<IDCompositionVisual> rootVisual; dcompDevice->CreateVisual(&rootVisual);
    ComPtr<IDCompositionVisual> mainVisual; dcompDevice->CreateVisual(&mainVisual);
    mainVisual->SetContent(swapChain.Get());
    rootVisual->AddVisual(mainVisual.Get(), FALSE, nullptr);

    ComPtr<IDCompositionVisual> titleVisual; dcompDevice->CreateVisual(&titleVisual);
    ComPtr<IDCompositionSurface> titleSurface;
    dcompDevice->CreateSurface(2560, 64, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &titleSurface);
    titleVisual->SetContent(titleSurface.Get());

    ComPtr<IDCompositionEffectGroup> titleEffectGroup; dcompDevice->CreateEffectGroup(&titleEffectGroup);
    titleEffectGroup->SetOpacity(1.0f); titleVisual->SetEffect(titleEffectGroup.Get());
    rootVisual->AddVisual(titleVisual.Get(), TRUE, mainVisual.Get());
    dcompTarget->SetRoot(rootVisual.Get()); dcompDevice->Commit();

    ComPtr<ID2D1Bitmap1> titleBitmap; POINT offset = { 0 }; ComPtr<IDXGISurface> dxgiTitleSurface;
    if (SUCCEEDED(titleSurface->BeginDraw(nullptr, __uuidof(IDXGISurface), &dxgiTitleSurface, &offset))) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        d2dContext->CreateBitmapFromDxgiSurface(dxgiTitleSurface.Get(), &bp, &titleBitmap);
        d2dContext->SetTarget(titleBitmap.Get());
        d2dContext->BeginDraw(); d2dContext->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f)); 
        ComPtr<ID2D1SolidColorBrush> bText; d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.0f), &bText);
        d2dContext->DrawTextW(L" DWM 沉浸架构: D2D 原生动画与大满贯", 34, titleFormat.Get(), D2D1::RectF(g_MenuStartX, 8.0f, 2560.0f, 64.0f), bText.Get());
        d2dContext->EndDraw(); titleSurface->EndDraw();
    }

    ComPtr<ID2D1Bitmap1> d2dTargetBitmap;
    ComPtr<ID2D1SolidColorBrush> brushGrid, brushAim, brushHit, brushMenuText;

     // 【修复】：网格变暗，文字变暗（因为底色是白色/浅色背景）
     d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f), &brushGrid); 
     d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.6f, 0.2f), &brushAim); 
     d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.2f, 0.0f), &brushHit); 
     d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f), &brushMenuText); 

    std::unique_ptr<ImGuiBridge> guiBridge = std::make_unique<ImGuiBridgeImpl>();
    guiBridge->Init(g_hCanvas, d3dDevice.Get(), d3dContext.Get());

    auto CreateTarget = [&]() {
        ComPtr<IDXGISurface> dxgiBackBuffer; swapChain->GetBuffer(0, __uuidof(IDXGISurface), &dxgiBackBuffer);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        d2dContext->CreateBitmapFromDxgiSurface(dxgiBackBuffer.Get(), &bp, &d2dTargetBitmap);
        d2dContext->SetTarget(d2dTargetBitmap.Get());
        guiBridge->ResizeOffscreenTarget(d3dDevice.Get(), d2dContext.Get(), currentWidth, currentHeight);
        };
    CreateTarget();

    bool showGrid = true; float aimRadius = 18.0f; int aimStyle = 0;
    std::vector<D2D1_POINT_2F> hitMarks;

    bool isTitleHidden = false;
    float menuAlpha = 0.0f; float targetMenuAlpha = 0.0f;
    float menuHoverAlphas[g_MenuCount] = { 0 };

    g_GpuInitDoneEvent.Notify();
    LONG currentRenderSignal = g_RenderEvent.Capture();
    uint64_t lastDrawnMousePos = 0;

    while (g_isRunning.load(std::memory_order_relaxed)) {

        bool queueHasCmds = !g_CommandQueue.is_empty();
        uint64_t currentMousePos = g_LastMousePosPacked.load(std::memory_order_relaxed);
        bool mouseMoved = (currentMousePos != lastDrawnMousePos);
        bool mouseInCanvas = g_bMouseInCanvas.load(std::memory_order_relaxed);

        bool isAnimating = std::abs(menuAlpha - targetMenuAlpha) > 0.01f;
        for (int i = 0; i < g_MenuCount; ++i) { if (menuHoverAlphas[i] > 0.01f && menuHoverAlphas[i] < 0.99f) isAnimating = true; }

        if (!queueHasCmds && !(mouseMoved && mouseInCanvas)) {
            if (isAnimating) g_RenderEvent.HybridWait(currentRenderSignal, 1500, 16);
            else g_RenderEvent.HybridWait(currentRenderSignal, 1500, INFINITE);
            LONG newRenderSignal = g_RenderEvent.Capture();
            if (newRenderSignal == currentRenderSignal && !isAnimating) continue;
            currentRenderSignal = newRenderSignal;
        }

        if (!g_isRunning.load(std::memory_order_relaxed)) break;

        int pendingResizeW = -1, pendingResizeH = -1;

        RenderCommand cmd;
        while (g_CommandQueue.pop(cmd)) {
            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, CmdResize>) {
                    if (arg.width > 0 && arg.height > 0) { pendingResizeW = arg.width; pendingResizeH = arg.height; }
                }
                else if constexpr (std::is_same_v<T, CmdChangeColor>) brushAim->SetColor(D2D1::ColorF(arg.r, arg.g, arg.b));
                else if constexpr (std::is_same_v<T, CmdSetGrid>) showGrid = arg.show;
                else if constexpr (std::is_same_v<T, CmdSetAimRadius>) aimRadius = arg.radius;
                else if constexpr (std::is_same_v<T, CmdSetAimStyle>) aimStyle = arg.style;
                else if constexpr (std::is_same_v<T, CmdAddHitMark>) hitMarks.push_back({ arg.x, arg.y });
                else if constexpr (std::is_same_v<T, CmdResetCanvas>) hitMarks.clear();
                else if constexpr (std::is_same_v<T, CmdKeyDown>) {
                    if (arg.key == 'C' || arg.key == 'c') hitMarks.clear();
                    if (arg.key == VK_SPACE) {
                        POINT pt; GetCursorPos(&pt); ScreenToClient(g_hCanvas, &pt);
                        if (pt.x > (int)(200 * g_dpiScale)) hitMarks.push_back({ (float)pt.x / g_dpiScale, (float)pt.y / g_dpiScale });
                    }
                }
                else if constexpr (std::is_same_v<T, CmdWin32Msg>) guiBridge->HandleWin32Message(arg.hwnd, arg.msg, arg.wParam, arg.lParam);
                else if constexpr (std::is_same_v<T, CmdExit>) g_isRunning.store(false, std::memory_order_relaxed);
                }, cmd);
        }

        if (pendingResizeW > 0 && pendingResizeH > 0 && (pendingResizeW != currentWidth || pendingResizeH != currentHeight)) {
            d2dContext->SetTarget(nullptr); d2dTargetBitmap.Reset();
            d2dContext->Flush(); ComPtr<ID3D11DeviceContext> d3dContext2; d3dDevice->GetImmediateContext(&d3dContext2);
            d3dContext2->ClearState(); d3dContext2->Flush();
            if (SUCCEEDED(swapChain->ResizeBuffers(2, pendingResizeW, pendingResizeH, DXGI_FORMAT_UNKNOWN, 0))) {
                currentWidth = pendingResizeW; currentHeight = pendingResizeH; CreateTarget();
            }
        }
        if (!g_isRunning.load(std::memory_order_relaxed)) break;

        bool wantHideTitle = g_IsTitleMenuVisible.load(std::memory_order_relaxed);
        if (wantHideTitle != isTitleHidden) {
            isTitleHidden = wantHideTitle;
            ComPtr<IDCompositionAnimation> animation; dcompDevice->CreateAnimation(&animation);
            float targetA = isTitleHidden ? 0.0f : 1.0f;
            animation->AddCubic(0.0, 0.0f, 0.0f, 0.0f, targetA); animation->End(0.2, targetA);
            titleEffectGroup->SetOpacity(animation.Get()); dcompDevice->Commit();
        }

        targetMenuAlpha = isTitleHidden ? 1.0f : 0.0f;
        menuAlpha += (targetMenuAlpha - menuAlpha) * 0.2f;

        guiBridge->RenderFrame(d3dContext.Get(), menuAlpha, showGrid);

        d2dContext->SetTarget(d2dTargetBitmap.Get());
        d2dContext->BeginDraw();
        d2dContext->SetDpi(96.0f * g_dpiScale, 96.0f * g_dpiScale);

        d2dContext->Clear(D2D1::ColorF(0.95f, 0.95f, 0.98f, 1.0f)); 

        float logicalW = currentWidth / g_dpiScale; float logicalH = currentHeight / g_dpiScale;
        float captionHeightLogical = g_CaptionHeight.load() / g_dpiScale;

        if (showGrid) {
            for (float x = 0; x < logicalW; x += 40.0f) d2dContext->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, logicalH), brushGrid.Get());
            for (float y = 0; y < logicalH; y += 40.0f) d2dContext->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(logicalW, y), brushGrid.Get());
        }

        for (const auto& hit : hitMarks) {
            d2dContext->DrawLine(D2D1::Point2F(hit.x - 6, hit.y - 6), D2D1::Point2F(hit.x + 6, hit.y + 6), brushHit.Get(), 3.0f);
            d2dContext->DrawLine(D2D1::Point2F(hit.x - 6, hit.y + 6), D2D1::Point2F(hit.x + 6, hit.y - 6), brushHit.Get(), 3.0f);
        }

        POINT pt; GetCursorPos(&pt); ScreenToClient(g_hCanvas, &pt);
        float mouseLogicalX = (float)pt.x / g_dpiScale;
        float mouseLogicalY = (float)pt.y / g_dpiScale;

        if (menuAlpha > 0.01f) {
            float startX = g_MenuStartX;
            for (int i = 0; i < g_MenuCount; ++i) {
                float itemWidth = g_MenuItems[i].width;
                bool isHover = (mouseInCanvas && mouseLogicalY <= captionHeightLogical && mouseLogicalX >= startX && mouseLogicalX < startX + itemWidth);
                menuHoverAlphas[i] += ((isHover ? 1.0f : 0.0f) - menuHoverAlphas[i]) * 0.25f;

                if (menuHoverAlphas[i] > 0.01f) {
                    ComPtr<ID2D1SolidColorBrush> bgBrush;
                    d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.1f * menuAlpha * menuHoverAlphas[i]), &bgBrush);
                    d2dContext->FillRectangle(D2D1::RectF(startX, 0, startX + itemWidth, captionHeightLogical), bgBrush.Get());
                }

                brushMenuText->SetOpacity(menuAlpha);
                d2dContext->DrawTextW(g_MenuItems[i].text, static_cast<UINT32>(wcslen(g_MenuItems[i].text)), menuFormat.Get(),
                    D2D1::RectF(startX + g_MenuPadding / 2.0f, 6.0f, startX + itemWidth, captionHeightLogical), brushMenuText.Get());
                startX += itemWidth;
            }
        }

        if (ID2D1Bitmap1* imguiBmp = guiBridge->GetD2DTexture()) {
            d2dContext->DrawBitmap(imguiBmp, D2D1::RectF(0, 0, logicalW, logicalH), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        lastDrawnMousePos = currentMousePos;

        if (mouseInCanvas && pt.x > (int)(200 * g_dpiScale) && pt.y > g_CaptionHeight.load() && !g_ImGuiWantCaptureMouse.load(std::memory_order_relaxed)) {
            float fx = mouseLogicalX, fy = mouseLogicalY;
            if (aimStyle == 0) {
                d2dContext->DrawLine(D2D1::Point2F(fx - aimRadius - 7, fy), D2D1::Point2F(fx + aimRadius + 7, fy), brushAim.Get(), 2.0f);
                d2dContext->DrawLine(D2D1::Point2F(fx, fy - aimRadius - 7), D2D1::Point2F(fx, fy + aimRadius + 7), brushAim.Get(), 2.0f);
                d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(fx, fy), aimRadius, aimRadius), brushAim.Get(), 2.0f);
            }
            else if (aimStyle == 1) {
                ComPtr<ID2D1SolidColorBrush> fillBrush; d2dContext->CreateSolidColorBrush(brushAim->GetColor(), &fillBrush);
                d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fx, fy), aimRadius * 0.3f, aimRadius * 0.3f), fillBrush.Get());
            }
            else if (aimStyle == 2) {
                d2dContext->DrawLine(D2D1::Point2F(fx, fy - aimRadius), D2D1::Point2F(fx - aimRadius, fy + aimRadius), brushAim.Get(), 2.0f);
                d2dContext->DrawLine(D2D1::Point2F(fx - aimRadius, fy + aimRadius), D2D1::Point2F(fx + aimRadius, fy + aimRadius), brushAim.Get(), 2.0f);
                d2dContext->DrawLine(D2D1::Point2F(fx + aimRadius, fy + aimRadius), D2D1::Point2F(fx, fy - aimRadius), brushAim.Get(), 2.0f);
                d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(fx, fy), 2.0f, 2.0f), brushAim.Get(), 2.0f);
            }
        }

        d2dContext->EndDraw();
        swapChain->Present(1, 0);
    }

    if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
    guiBridge->Shutdown();
    AttachThreadInput(renderThreadId, g_UIThreadId, FALSE);
}

// =========================================================
// UI 线程：原生 Win32 右键菜单生成器 
// =========================================================
void ShowCustomWin32Menu(HWND hwnd, int selectedIndex) {
    if (selectedIndex < 0 || selectedIndex >= g_MenuCount) return;

    HMENU hMenu = CreatePopupMenu();
    if (selectedIndex == 0) {
        AppendMenuW(hMenu, MF_STRING, 3001, L"新建画布 (New)\tCtrl+N");
        HMENU hRecent = CreatePopupMenu();
        AppendMenuW(hRecent, MF_STRING, 3002, L"项目: 赛博空间");
        AppendMenuW(hRecent, MF_STRING, 3003, L"项目: 极简打靶场");
        AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hRecent, L"打开近期 (Open Recent) ►");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, 2002, L"退出 (Exit)\tAlt+F4");
    }
    else if (selectedIndex == 1) {
        AppendMenuW(hMenu, MF_STRING, 2001, L"撤销弹孔 (Undo)\tCtrl+Z");
        AppendMenuW(hMenu, MF_STRING, 3004, L"清空所有 (Clear All)\tC");
    }
    else if (selectedIndex == 2) {
        AppendMenuW(hMenu, MF_STRING | MF_CHECKED, 3005, L"显示网格 (Show Grid)");
        AppendMenuW(hMenu, MF_STRING, 3006, L"性能监控面板 (Perf Overlay)");
    }
    else if (selectedIndex == 3) {
        AppendMenuW(hMenu, MF_STRING, 3007, L"关于 D2D 引擎 (About)...");
    }

    float popupX = g_MenuStartX;
    for (int i = 0; i < selectedIndex; ++i) popupX += g_MenuItems[i].width;

    POINT pt = { (int)(popupX * g_dpiScale), g_CaptionHeight.load() };
    ClientToScreen(hwnd, &pt);
    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, pt.x, pt.y, 0, g_hMainWindow, NULL);
    DestroyMenu(hMenu);
}

// =========================================================
// UI 线程：底层全屏 Canvas 回调
// =========================================================
LRESULT CALLBACK CanvasWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MOUSEWHEEL || msg == WM_CHAR) {
        g_CommandQueue.push(CmdWin32Msg{ hwnd, msg, wParam, lParam });
    }

    static POINT s_mouseDownPt = { 0, 0 };
    static bool s_isCaptionClicking = false;

    switch (msg) {
    case WM_CREATE: ImmAssociateContext(hwnd, NULL); return 0;
    case WM_ERASEBKGND: return 1;

        // 【重磅修复：HTTRANSPARENT 归位！】
        // 将 DWM 三大金刚键的控制权完美透传给底层的主窗口，彻底复活关闭/最大化！
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }; ScreenToClient(hwnd, &pt);

        // 只有在非交互菜单的顶部标题栏区域，才允许透传，让底层主窗口处理！
        if (pt.y <= g_CaptionHeight.load()) {
            float logicalX = (float)pt.x / g_dpiScale;
            // 如果鼠标在我们的自定义菜单上，阻断透传，由 Canvas 自己处理！
            if (logicalX >= g_MenuStartX && logicalX <= g_MenuStartX + GetTotalMenuWidth()) {
                return HTCLIENT;
            }
            // 其他区域（包括右上角的三大金刚键和拖拽区），全部透传给底层主窗口！
            return HTTRANSPARENT;
        }
        return HTCLIENT;
    }

    case WM_MOUSEMOVE: {
        if (!g_bMouseInCanvas.load(std::memory_order_relaxed)) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tme);
            g_bMouseInCanvas.store(true, std::memory_order_relaxed);
        }

        int y = GET_Y_LPARAM(lParam); int x = GET_X_LPARAM(lParam);
        if (y <= g_CaptionHeight.load() && x > S(200)) { g_IsTitleMenuVisible.store(true, std::memory_order_relaxed); }
        else { g_IsTitleMenuVisible.store(false, std::memory_order_relaxed); }

        uint64_t packed = ((uint64_t)y << 32) | (uint64_t)(uint32_t)x;
        g_LastMousePosPacked.store(packed, std::memory_order_relaxed);
        g_RenderEvent.Notify();
        return 0;
    }

    case WM_MOUSELEAVE: {
        g_bMouseInCanvas.store(false, std::memory_order_relaxed);
        g_IsTitleMenuVisible.store(false, std::memory_order_relaxed);
        g_RenderEvent.Notify(); return 0;
    }

    case WM_LBUTTONDOWN: {
        int y = GET_Y_LPARAM(lParam); int x = GET_X_LPARAM(lParam);

        SetFocus(hwnd);
        if (x > S(200) && !g_ImGuiWantCaptureMouse.load(std::memory_order_relaxed)) {
            // 只在真实画布区打靶
            if (y > g_CaptionHeight.load()) {
                g_CommandQueue.push(CmdAddHitMark{ (float)x / g_dpiScale, (float)y / g_dpiScale });
            }
        }
        g_RenderEvent.Notify(); return 0;
    }

    case WM_LBUTTONUP: {
        int y = GET_Y_LPARAM(lParam); int x = GET_X_LPARAM(lParam);
        // 这里只处理菜单点击（因为拖拽已经被 HTTRANSPARENT 交给主窗口了！）
        if (y <= g_CaptionHeight.load() && x > S(200)) {
            int menuIndex = GetMenuIndexFromLogicalX((float)x / g_dpiScale);
            if (menuIndex != -1) ShowCustomWin32Menu(hwnd, menuIndex);
        }
        return 0;
    }

    case WM_SYSKEYDOWN: {
        if (wParam == 'F') { ShowCustomWin32Menu(hwnd, 0); return 0; }
        if (wParam == 'E') { ShowCustomWin32Menu(hwnd, 1); return 0; }
        if (wParam == 'V') { ShowCustomWin32Menu(hwnd, 2); return 0; }
        if (wParam == 'H') { ShowCustomWin32Menu(hwnd, 3); return 0; }
        break;
    }

    case WM_KEYDOWN: {
        if (wParam == 'C' || wParam == 'c') { g_CommandQueue.push(CmdResetCanvas{}); g_RenderEvent.Notify(); return 0; }
        g_CommandQueue.push(CmdKeyDown{ wParam }); g_RenderEvent.Notify(); return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            if (pt.x > S(200) && pt.y > g_CaptionHeight.load() && !g_ImGuiWantCaptureMouse.load(std::memory_order_relaxed)) {
                SetCursor(NULL); return TRUE;
            }
            else {
                SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE;
            }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// =========================================================
// UI 线程：顶层 Panel (完全自适应 DPI)
// =========================================================
LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int y = S(10);
        HWND hBtns[10];
        hBtns[0] = CreateWindowW(L"STATIC", L"全栈 C++ 降维控件秀", WS_CHILD | WS_VISIBLE, S(15), y, S(170), S(20), hwnd, NULL, NULL, NULL); y += S(25);
        hBtns[1] = CreateWindowW(L"BUTTON", L"科技绿 (Green)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, S(15), y, S(170), S(20), hwnd, (HMENU)101, NULL, NULL); y += S(22);
        hBtns[2] = CreateWindowW(L"BUTTON", L"火红 (Red)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, S(15), y, S(170), S(20), hwnd, (HMENU)102, NULL, NULL); y += S(22);
        hBtns[3] = CreateWindowW(L"BUTTON", L"深蓝 (Blue)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, S(15), y, S(170), S(20), hwnd, (HMENU)103, NULL, NULL); y += S(25);
        CheckRadioButton(hwnd, 101, 103, 101);

        hBtns[4] = CreateWindowW(L"STATIC", L"准星样式:", WS_CHILD | WS_VISIBLE, S(15), y, S(170), S(15), hwnd, NULL, NULL, NULL); y += S(20);
        HWND hCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, S(15), y, S(170), S(200), hwnd, (HMENU)106, NULL, NULL); y += S(30);
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"0 - 赛博十字圈"); SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"1 - 极简圆点"); SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)L"2 - 战术三角");
        SendMessage(hCombo, CB_SETCURSEL, 0, 0);

        hBtns[5] = CreateWindowW(L"STATIC", L"准星半径 (Radius):", WS_CHILD | WS_VISIBLE, S(15), y, S(170), S(15), hwnd, NULL, NULL, NULL); y += S(20);
        HWND hSlider = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, S(5), y, S(190), S(30), hwnd, (HMENU)105, NULL, NULL); y += S(35);
        SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(5, 80)); SendMessage(hSlider, TBM_SETPOS, TRUE, 18);

        hBtns[6] = CreateWindowW(L"BUTTON", L"显示背景网格", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, S(15), y, S(170), S(20), hwnd, (HMENU)104, NULL, NULL); y += S(25);
        CheckDlgButton(hwnd, 104, BST_CHECKED);

        hBtns[7] = CreateWindowW(L"BUTTON", L"清理弹孔 (或按C键)", WS_CHILD | WS_VISIBLE, S(15), y, S(170), S(30), hwnd, (HMENU)107, NULL, NULL); y += S(40);

        hBtns[8] = CreateWindowW(L"STATIC", L"UIAnimation 硬件加速:", WS_CHILD | WS_VISIBLE, S(15), y, S(170), S(15), hwnd, NULL, NULL, NULL); y += S(20);
        hBtns[9] = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, S(15), y, S(170), S(20), hwnd, (HMENU)108, NULL, NULL);

        UpdateGlobalFont();
        for (auto h : hBtns) SendMessage(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(hCombo, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessage(hSlider, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        InitUIAnimation(); StartProgressPingPongAnimation(); return 0;
    }

    case WM_UPDATE_PROGRESS: {
        SendDlgItemMessage(hwnd, 108, PBM_SETPOS, (int)(double)wParam, 0); return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam); int notify = HIWORD(wParam);
        if (id == 101) g_CommandQueue.push(CmdChangeColor{ 0.0f, 1.0f, 0.5f });
        if (id == 102) g_CommandQueue.push(CmdChangeColor{ 1.0f, 0.2f, 0.2f });
        if (id == 103) g_CommandQueue.push(CmdChangeColor{ 0.2f, 0.5f, 1.0f });
        if (id == 104) g_CommandQueue.push(CmdSetGrid{ IsDlgButtonChecked(hwnd, 104) == BST_CHECKED });
        if (id == 107) { g_CommandQueue.push(CmdResetCanvas{}); SetFocus(g_hCanvas); }
        if (id == 106 && notify == CBN_SELCHANGE) { g_CommandQueue.push(CmdSetAimStyle{ (int)SendDlgItemMessage(hwnd, 106, CB_GETCURSEL, 0, 0) }); SetFocus(g_hCanvas); }
        g_RenderEvent.Notify(); return 0;
    }
    case WM_HSCROLL: {
        if ((HWND)lParam == GetDlgItem(hwnd, 105)) { g_CommandQueue.push(CmdSetAimRadius{ (float)SendMessage((HWND)lParam, TBM_GETPOS, 0, 0) }); g_RenderEvent.Notify(); }
        return 0;
    }
    case WM_ERASEBKGND: { HDC hdc = (HDC)wParam; RECT rc; GetClientRect(hwnd, &rc); FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW)); return 1; }
    case WM_SETCURSOR: { SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE; }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// =========================================================
// UI 线程：主窗口回调 (接管 DWM 逻辑)
// =========================================================
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_dpiScale = GetDpiForWindow(hwnd) / 96.0f;
        g_CaptionHeight.store(compute_standard_caption_height_for_window(hwnd), std::memory_order_relaxed);
        MARGINS margins = { 0, 0, 0, 1 }; DwmExtendFrameIntoClientArea(hwnd, &margins);
        return 0;
    }
    case WM_NCCALCSIZE: {
        // 剥夺原本的丑陋边框，并通知 DWM 重算客户区
        if (wParam == TRUE) {
            auto parameters = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            auto& r = parameters->rgrc[0];
            int dpi = GetDpiForWindow(hwnd);
            int frameX = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            int frameY = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            r.right -= frameX; r.left += frameX; r.bottom -= frameY;
            return 0;
        }
        break;
    }

    case WM_NCHITTEST: {
        LRESULT hit;
        // 必须先让 DWM 处理，它才能识别出右上角的三大按钮热区！
        if (DwmDefWindowProc(hwnd, msg, wParam, lParam, &hit)) return hit;

        // DWM 不管的，我们用你的热区算法自己算！(完美实现拖拽与八向缩放)
        hit = compute_sector_of_window(hwnd, wParam, lParam, g_CaptionHeight.load());
        if (hit != HTNOWHERE) return hit;

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    case WM_DPICHANGED: {
        g_dpiScale = HIWORD(wParam) / 96.0f;
        g_CaptionHeight.store(compute_standard_caption_height_for_window(hwnd), std::memory_order_relaxed);
        UpdateGlobalFont(); // 刷新字体大小
        RECT* const prcNewWindow = (RECT*)lParam;
        SetWindowPos(hwnd, NULL, prcNewWindow->left, prcNewWindow->top, prcNewWindow->right - prcNewWindow->left, prcNewWindow->bottom - prcNewWindow->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == 3004 || LOWORD(wParam) == 2001) { g_CommandQueue.push(CmdResetCanvas{}); g_RenderEvent.Notify(); }
        if (LOWORD(wParam) == 2002) { DestroyWindow(hwnd); }
        return 0;
    }
    case WM_SIZE: {
        int width = max(LOWORD(lParam), 1); int height = max(HIWORD(lParam), 1);
        if (g_hCanvas && g_hPanel) {
            HDWP hdwp = BeginDeferWindowPos(2);
            int panelW = S(200);
            DeferWindowPos(hdwp, g_hCanvas, HWND_BOTTOM, 0, 0, width, height, 0);
            DeferWindowPos(hdwp, g_hPanel, HWND_TOP, 0, 0, panelW, height, 0);
            EndDeferWindowPos(hdwp);
        }
        g_CommandQueue.push(CmdResize{ width, height }); g_RenderEvent.Notify(); return 0;
    }
    case WM_DESTROY:
        g_CommandQueue.push(CmdExit{}); g_isRunning.store(false, std::memory_order_relaxed);
        g_RenderEvent.Notify(); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 主程序入口
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icex);

    // 【首次初始化排版】：先算出物理文字宽度
    CalculateMenuWidths();

    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSW wcMain = { 0 }; wcMain.lpfnWndProc = MainWndProc; wcMain.hInstance = hInstance; wcMain.hCursor = LoadCursor(NULL, IDC_ARROW); wcMain.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wcMain.lpszClassName = L"MainClass"; RegisterClassW(&wcMain);
    WNDCLASSW wcCanvas = { 0 }; wcCanvas.lpfnWndProc = CanvasWndProc; wcCanvas.hInstance = hInstance; wcCanvas.hbrBackground = NULL; wcCanvas.lpszClassName = L"CanvasClass"; RegisterClassW(&wcCanvas);
    WNDCLASSW wcPanel = { 0 }; wcPanel.lpfnWndProc = PanelWndProc; wcPanel.hInstance = hInstance; wcPanel.hbrBackground = NULL; wcPanel.lpszClassName = L"PanelClass"; RegisterClassW(&wcPanel);

    // 【完美适配高分屏】：初始宽度高度调大为 1280x800，并自动按屏幕 DPI 放大！
    int initW = 1280; int initH = 800;
    HDC screenDC = GetDC(NULL); int sysDpi = GetDeviceCaps(screenDC, LOGPIXELSX); ReleaseDC(NULL, screenDC);
    initW = initW * sysDpi / 96; initH = initH * sysDpi / 96;

    g_hMainWindow = CreateWindowExW(0, wcMain.lpszClassName, L"Modern C++20 Win32 究极大满贯", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, initW, initH, NULL, NULL, hInstance, NULL);
    g_hCanvas = CreateWindowExW(0, wcCanvas.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, initW, initH, g_hMainWindow, NULL, hInstance, NULL);
    g_hPanel = CreateWindowExW(0, wcPanel.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, S(200), initH, g_hMainWindow, NULL, hInstance, NULL);

    g_UIThreadId = GetCurrentThreadId();

    std::thread renderThread(RenderThreadFunc);
    g_GpuInitDoneEvent.Wait(0);

    RECT rc; GetClientRect(g_hCanvas, &rc);
    g_CommandQueue.push(CmdResize{ (int)rc.right, (int)rc.bottom });
    g_RenderEvent.Notify();

    ShowWindow(g_hMainWindow, SW_SHOW); UpdateWindow(g_hMainWindow);

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    if (renderThread.joinable()) renderThread.join();
    if (g_hFont) DeleteObject(g_hFont);
    return 0;
}