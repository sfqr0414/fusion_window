#include <atomic>
#include <format>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <syncstream>
#include <source_location>
#include <type_traits>

namespace utils {

	inline std::string timestamp_prefix() {
		auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
		std::chrono::zoned_time zt{ std::chrono::current_zone(), now };
		return std::format("[{:%Y-%m-%d %H:%M:%S}] ", zt);
	}

	template <typename T>
	class lock_free_stack {
	private:
		struct node {
			node(T const& data_) : data(data_), next(nullptr) {}
			node(T&& data_) : data(std::move(data_)), next(nullptr) {}

			T data;
			node* next;
		};

		std::atomic<unsigned> threads_in_pop;
		std::atomic<node*> to_be_deleted;

		static void delete_nodes(node* nodes) {
			while (nodes) {
				node* next = nodes->next;
				delete nodes;
				nodes = next;
			}
		}

		void chain_pending_nodes(node* nodes) {
			node* last = nodes;
			while (node* const next = last->next) { last = next; }
			chain_pending_nodes(nodes, last);
		}

		void chain_pending_nodes(node* first, node* last) {
			last->next = to_be_deleted.load(std::memory_order_relaxed);
			while (!to_be_deleted.compare_exchange_weak(last->next, first, std::memory_order_release, std::memory_order_relaxed));
		}

		void chain_pending_node(node* n) { chain_pending_nodes(n, n); }

		void try_reclaim(node* old_head) {
			if (threads_in_pop == 1) {
				node* nodes_to_delete = to_be_deleted.exchange(nullptr, std::memory_order_acquire);
				if (!--threads_in_pop) { delete_nodes(nodes_to_delete); }
				else if (nodes_to_delete) { chain_pending_nodes(nodes_to_delete); }
				delete old_head;
			}
			else {
				chain_pending_node(old_head);
				--threads_in_pop;
			}
		}

		std::atomic<node*> head;
		std::atomic<std::size_t> m_size;

	public:
		lock_free_stack() : head(nullptr), m_size(0), threads_in_pop(0), to_be_deleted(nullptr) {}
		~lock_free_stack() {
			while (pop());
			delete_nodes(to_be_deleted.load(std::memory_order_acquire));
		}

		std::size_t size() { return m_size.load(std::memory_order_relaxed); }
		bool empty() { return head.load(std::memory_order_relaxed) == nullptr; }

		void push(T const& data) {
			node* const new_node = new node(data);
			new_node->next = head.load(std::memory_order_relaxed);
			while (!head.compare_exchange_weak(new_node->next, new_node, std::memory_order_release, std::memory_order_relaxed));
			m_size.fetch_add(1, std::memory_order_relaxed);
		}

		void push(T&& data) {
			node* const new_node = new node(std::move(data));
			new_node->next = head.load(std::memory_order_relaxed);
			while (!head.compare_exchange_weak(new_node->next, new_node, std::memory_order_release, std::memory_order_relaxed));
			m_size.fetch_add(1, std::memory_order_relaxed);
		}

		std::optional<T> pop() {
			++threads_in_pop;
			node* old_head = head.load(std::memory_order_relaxed);
			while (old_head && !head.compare_exchange_weak(old_head, old_head->next, std::memory_order_acquire, std::memory_order_relaxed));
			std::optional<T> res(std::nullopt);
			if (old_head) {
				m_size.fetch_sub(1, std::memory_order_relaxed);
				res = std::move(old_head->data);
				try_reclaim(old_head);
			}
			else { --threads_in_pop; }
			return res;
		}


		void pop_all(std::vector<T>& batch) {
			node* current = head.exchange(nullptr, std::memory_order_acquire);
			if (!current) return;

			std::size_t count = 0;
			while (current) {
				batch.push_back(std::move(current->data));
				node* next = current->next;
				delete current;
				current = next;
				count++;
			}
			m_size.fetch_sub(count, std::memory_order_relaxed);
		}
	};

	class async_print {
	public:
		using PrefixFunc = std::string(*)();
		using PostPrintFunc = void(*)();

		struct info {
			std::ostream* os;
			std::source_location loc;
			std::string prefix;
			std::string message;
		};

		static async_print& get() {
			static async_print instance;
			return instance;
		}

		void set_cout_color(std::string_view color) { cout_color.store(color.empty() ? "" : color.data(), std::memory_order_release); }
		void set_cerr_color(std::string_view color) { cerr_color.store(color.empty() ? "" : color.data(), std::memory_order_release); }
		void set_static_prefix(std::string_view prefix) { static_prefix.store(prefix.empty() ? "" : prefix.data(), std::memory_order_release); }
		void set_prefix_callback(PrefixFunc func) { prefix_func.store(func, std::memory_order_release); }
		void set_print_location(bool enable) { print_source_location.store(enable, std::memory_order_release); }
		void set_post_print_callback(PostPrintFunc func) { post_print_func.store(func, std::memory_order_release); }

		void enqueue(std::ostream* os, std::source_location loc, std::string message) {
			std::string pfx;
			if (auto func = prefix_func.load(std::memory_order_acquire)) {
				pfx = func();
			}
			else {
				const char* sp = static_prefix.load(std::memory_order_acquire);
				if (sp != nullptr && sp[0] != '\0') pfx = sp;
			}

			message_queue.push(info{ os, loc, std::move(pfx), std::move(message) });

			int expected = 0;
			if (work_available.compare_exchange_strong(expected, 1, std::memory_order_release, std::memory_order_relaxed)) {
				work_available.notify_one();
			}
		}

	private:
		async_print() : print_thread([this](std::stop_token st) { loop(st); }) {
			batch.reserve(2048);
		}

		~async_print() {
			print_thread.request_stop();
			work_available.store(1, std::memory_order_release);
			work_available.notify_all();

			if (print_thread.joinable()) {
				print_thread.join();
			}
			drain_and_print();
		}

		void loop(std::stop_token st) {
			while (!st.stop_requested()) {
				work_available.wait(0, std::memory_order_acquire);
				if (st.stop_requested()) break;

				if (work_available.exchange(0, std::memory_order_acquire) != 0) {
					drain_and_print();
				}
			}
			drain_and_print();
		}

		void drain_and_print() {
			message_queue.pop_all(batch);
			if (batch.empty()) return;

			bool flush_cout = false;
			bool flush_cerr = false;
			const char* reset_color = "\033[0m";

			for (const auto& item : batch | std::views::reverse) {
				if (!item.os) continue;


				bool p_loc = print_source_location.load(std::memory_order_relaxed);
				PostPrintFunc post_print = post_print_func.load(std::memory_order_relaxed);


				if (item.os == &std::cout) {
					const char* color = cout_color.load(std::memory_order_relaxed);
					bool use_color = (color != nullptr && color[0] != '\0');

					if (use_color) std::cout << color;
					if (!item.prefix.empty()) std::cout << item.prefix;
					if (p_loc) std::cout << "[" << item.loc.file_name() << ":" << item.loc.line() << ":" << item.loc.column() << " " << item.loc.function_name() << "] ";

					std::cout << item.message;

					if (use_color) std::cout << reset_color;
					flush_cout = true;
				}
				else if (item.os == &std::cerr) {
					const char* color = cerr_color.load(std::memory_order_relaxed);
					bool use_color = (color != nullptr && color[0] != '\0');

					if (use_color) std::cerr << color;
					if (!item.prefix.empty()) std::cerr << item.prefix;
					if (p_loc) std::cerr << "[" << item.loc.file_name() << ":" << item.loc.line() << ":" << item.loc.column() << " " << item.loc.function_name() << "] ";

					std::cerr << item.message;

					if (use_color) std::cerr << reset_color;
					flush_cerr = true;
				}
				else {
					if (!item.prefix.empty()) *(item.os) << item.prefix;
					if (p_loc) *(item.os) << "[" << item.loc.file_name() << ":" << item.loc.line() << ":" << item.loc.column() << " " << item.loc.function_name() << "] ";

					*(item.os) << item.message;
					item.os->flush();
				}

				if (post_print) {
					post_print();
				}
			}

			if (flush_cout) std::cout.flush();
			if (flush_cerr) std::cerr.flush();

			batch.clear();
		}

		utils::lock_free_stack<info> message_queue;
		std::atomic<int> work_available{ 0 };
		std::jthread print_thread;
		std::vector<info> batch;

		std::atomic<const char*> cout_color{ "\033[93m" };
		std::atomic<const char*> cerr_color{ "\033[91m" };
		std::atomic<const char*> static_prefix{ "" };
		std::atomic<PrefixFunc> prefix_func{ nullptr };
		std::atomic<bool> print_source_location{ false };
		std::atomic<PostPrintFunc> post_print_func{ nullptr };
	};

	inline void set_log_prefix(std::string_view prefix) { async_print::get().set_static_prefix(prefix); }
	inline void set_log_prefix(std::string(*func)()) { async_print::get().set_prefix_callback(func); }
	inline void set_log_color_cout(std::string_view color) { async_print::get().set_cout_color(color); }
	inline void set_log_color_cerr(std::string_view color) { async_print::get().set_cerr_color(color); }
	inline void set_log_source_location(bool enable) { async_print::get().set_print_location(enable); }
	inline void set_log_post_print_callback(void(*func)()) { async_print::get().set_post_print_callback(func); }

	template <class T> struct AsStream { const T& v; };

	template <class T>
	decltype(auto) wrap(const T& arg) {
		if constexpr (requires { std::formatter<std::remove_cvref_t<T>, char>(); }) return arg;
		else return AsStream<T>{arg};
	}

	template <class... Args>
	using mapped_string = std::basic_format_string<char, decltype(wrap(std::declval<Args>()))...>;

	struct log_format {
		std::string_view fmt;
		std::source_location loc;

		template <class String>
			requires std::constructible_from<std::string_view, const String&>
		consteval log_format(const String& s, std::source_location loc = std::source_location::current())
			: fmt(s), loc(loc) {
		}
	};

	template<class... Args>
	auto format_to(mapped_string<Args...> fmt, Args&&... args) {
		return std::format(fmt, wrap(args)...);
	}

	template<class... Args>
	auto vformat_to(std::string_view fmt, Args&&... args) {
		auto wrapped = std::forward_as_tuple(wrap(args)...);
		return std::apply([&](auto&&... wargs) {
			return std::vformat(fmt, std::make_format_args(wargs...));
			}, wrapped);
	}

	template <typename... Args>
	void console(std::ostream& os, log_format fmt_loc, Args&&... args) {
		async_print::get().enqueue(&os, fmt_loc.loc, vformat_to(fmt_loc.fmt, std::forward<Args>(args)...));
	}

	template <class... Args>
	void console(log_format fmt_loc, Args&&... args) {
		async_print::get().enqueue(&std::cout, fmt_loc.loc, vformat_to(fmt_loc.fmt, std::forward<Args>(args)...));
	}
}

namespace std {
	template <class T>
	struct formatter<utils::AsStream<T>, char> {
		constexpr auto parse(auto& ctx) { return ctx.begin(); }
		auto format(const utils::AsStream<T>& s, auto& ctx) const {
			ostringstream os;
			os << s.v;
			return ranges::copy(os.str(), ctx.out()).out;
		}
	};
}


#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <uxtheme.h> 
#include <vssym32.h>
#include <avrt.h> 
#include <iostream>
#include <thread>
#include <atomic>
#include <variant>
#include <vector>
#include <array>
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

#include "ui/native_ui.hpp"

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
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

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
std::atomic<int> g_CaptionHeight{ 32 };
inline int S(int val) { return (int)(val * g_dpiScale + 0.5f); }

enum class UiCursorState : int {
	CanvasHidden = 0,
	Arrow = 1,
	Hand = 2,
	IBeam = 3,
};

std::atomic<int> g_UiCursorState{ static_cast<int>(UiCursorState::CanvasHidden) };
std::atomic<int> g_UiVisualLeft{ 0 };
std::atomic<int> g_UiVisualTop{ 0 };
std::atomic<int> g_UiVisualRight{ 0 };
std::atomic<int> g_UiVisualBottom{ 0 };

RECT GetCaptionButtonBounds(HWND hwnd) {
	RECT rc{ 0, 0, 0, 0 };
	DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &rc, sizeof(rc));
	return rc;
}

bool IsPointInCaptionButtons(HWND hwnd, POINT clientPt) {
	RECT rc = GetCaptionButtonBounds(hwnd);
	//utils::console("\nrc :(left:{},top:{},right:{},bottom:{}) PtInRect->{}\n\n", rc.left, rc.top, rc.right, rc.bottom, PtInRect(&rc, clientPt));
	return !IsRectEmpty(&rc) && PtInRect(&rc, clientPt);
}

void ExtendFrameIntoClient(HWND hwnd) {
	UINT dpi = GetDpiForWindow(hwnd);
	MARGINS margins{};
	margins.cyTopHeight = g_CaptionHeight.load(std::memory_order_relaxed);
	DwmExtendFrameIntoClientArea(hwnd, &margins);

	//MARGINS margins = { -1, -1, -1, -1 };
	//DwmExtendFrameIntoClientArea(hwnd, &margins);
}

inline auto compute_standard_caption_height_for_window(HWND window_handle) {
	auto const accounting_for_borders = 2;
	auto dpi = GetDpiForWindow(window_handle);
	RECT rcFrame = { 0 };
	AdjustWindowRectExForDpi(&rcFrame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
	return -rcFrame.top - accounting_for_borders;
}

ComPtr<IDWriteTextFormat> CreateCaptionTextFormat(IDWriteFactory* factory, HWND hwnd) {
	NONCLIENTMETRICSW ncm{ sizeof(ncm) };
	if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(hwnd))) {
		return nullptr;
	}
	// lfHeight 是设备像素（负值），DWrite fontSize 是 DIP（96dpi基准），必须换算
	float fontSize = (float)abs(ncm.lfCaptionFont.lfHeight) * 96.0f / (float)GetDpiForWindow(hwnd);
	if (fontSize < 8.0f) fontSize = 12.0f;
	ComPtr<IDWriteTextFormat> format;
	if (FAILED(factory->CreateTextFormat(ncm.lfCaptionFont.lfFaceName, NULL,
		static_cast<DWRITE_FONT_WEIGHT>(ncm.lfCaptionFont.lfWeight),
		ncm.lfCaptionFont.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL, fontSize, L"zh-cn", &format))) {
		return nullptr;
	}
	format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
	format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	return format;
}

inline auto compute_sector_of_window(HWND window_handle, WPARAM, LPARAM lparam, int caption_height) -> LRESULT {
	RECT window_rectangle;
	GetWindowRect(window_handle, &window_rectangle);
	auto offset = 10;
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

float GetMenuStartLogicalX();
float GetMenuEndLogicalX();

inline int GetPanelWidth() {
	return S(200);
}

inline int GetPanelContentTop() {
	return g_CaptionHeight.load(std::memory_order_relaxed) + S(10);
}

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
	float currentX = GetMenuStartLogicalX();
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
struct CmdKeyDown { WPARAM key; bool ctrl; bool shift; bool alt; };
struct CmdWin32Msg { HWND hwnd; UINT msg; WPARAM wParam; LPARAM lParam; };
struct CmdUpdateAnim { float menuAlpha; float progress; std::array<float, 4> hoverAlpha; };
struct CmdExit {};
struct CmdUpdateTitleBar {
	HICON hIcon;
	int iconSizePx;
	std::array<wchar_t, 256> title;
	RECT buttonRect;
};
using RenderCommand = std::variant<
	CmdResize, 
	CmdChangeColor, 
	CmdSetGrid, 
	CmdSetAimRadius, 
	CmdSetAimStyle, 
	CmdAddHitMark, 
	CmdResetCanvas, 
	CmdKeyDown, 
	CmdWin32Msg, 
	CmdUpdateAnim, 
	CmdUpdateTitleBar,
	CmdExit>;


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
FutexEvent g_ResizeDoneEvent; // [新增] 用于阻塞 UI 线程直到 Resize 帧上屏
std::atomic<bool> g_RenderThreadReady(false); // [新增] 防止窗口刚创建时死锁
std::atomic<bool> g_isRunning(true);
HWND g_hMainWindow = NULL, g_hCanvas = NULL, g_hPanel = NULL;
DWORD g_UIThreadId = 0;
std::atomic<bool> g_bMouseInCanvas(false);
std::atomic<uint64_t> g_LastMousePosPacked(0);
std::atomic<bool> g_ImGuiWantCaptureMouse(false);

bool g_MenuVisibleState = false;
int g_HoveredMenuIndex = -1;
std::atomic<bool> g_IsMenuPopupActive(false);
std::atomic<int> g_NextMenuIndex{-1};
std::atomic<bool> g_IsTitleMenuVisible(false);
std::atomic<int> g_PressedMenuIndexForRender{ -1 };

POINT g_CaptionPressPoint{ 0, 0 };
int g_PressedMenuIndex = -1;
bool g_IsCaptionPressActive = false;
bool g_IsCaptionDragStarted = false;


constexpr float kCaptionLogicalInset = 8.0f;
constexpr float kCaptionTextGap = 8.0f;

float GetMenuStartLogicalX() {
	return (float)GetPanelWidth() / g_dpiScale + kCaptionLogicalInset;
}

float GetMenuEndLogicalX() {
	return GetMenuStartLogicalX() + GetTotalMenuWidth();
}

bool IsInCustomCaptionBand(POINT clientPt) {
	//utils::console("\nPOINT client:({},{}) GetPanelWidth()={}\n\n", clientPt.x, clientPt.y, GetPanelWidth());
	return clientPt.y >= 0
		&& clientPt.y <= g_CaptionHeight.load(std::memory_order_relaxed)
		&& clientPt.x > GetPanelWidth()
		&& !IsPointInCaptionButtons(g_hMainWindow, clientPt);
}

bool IsInMenuBarBand(POINT clientPt) {
	if (!IsInCustomCaptionBand(clientPt)) {
		return false;
	}
	float logicalX = (float)clientPt.x / g_dpiScale;
	return logicalX >= GetMenuStartLogicalX() && logicalX < GetMenuEndLogicalX();
}

bool IsInDraggableCaptionBand(POINT clientPt) {
	return clientPt.y >= 0
		&& clientPt.y <= g_CaptionHeight.load(std::memory_order_relaxed)
		&& !IsInMenuBarBand(clientPt)
		&& !IsPointInCaptionButtons(g_hMainWindow, clientPt);
}

void UpdateTitleMenuVisibilityFromPoint(POINT clientPt, bool pointerInWindow) {
	bool shouldShowMenu = g_IsMenuPopupActive.load(std::memory_order_relaxed)
		|| (pointerInWindow && IsInCustomCaptionBand(clientPt));
	g_IsTitleMenuVisible.store(shouldShowMenu, std::memory_order_relaxed);
}

void UpdateTitleMenuVisibilityFromCursor(HWND hwnd) {
	POINT screenPt{};
	GetCursorPos(&screenPt);
	POINT clientPt = screenPt;
	ScreenToClient(hwnd, &clientPt);
	RECT clientRect{};
	GetClientRect(hwnd, &clientRect);
	UpdateTitleMenuVisibilityFromPoint(clientPt, PtInRect(&clientRect, clientPt));
	uint64_t packed = ((uint64_t)(uint32_t)clientPt.y << 32) | (uint64_t)(uint32_t)clientPt.x;
	g_LastMousePosPacked.store(packed, std::memory_order_relaxed);
}

ComPtr<ID2D1Bitmap1> CreateBitmapFromHicon(ID2D1DeviceContext* d2dContext, HICON hIcon, UINT size) {
	if (!d2dContext || !hIcon || size == 0) {
		return nullptr;
	}

	BITMAPV5HEADER bi{};
	bi.bV5Size = sizeof(bi);
	bi.bV5Width = static_cast<LONG>(size);
	bi.bV5Height = -static_cast<LONG>(size);
	bi.bV5Planes = 1;
	bi.bV5BitCount = 32;
	bi.bV5Compression = BI_BITFIELDS;
	bi.bV5RedMask = 0x00FF0000;
	bi.bV5GreenMask = 0x0000FF00;
	bi.bV5BlueMask = 0x000000FF;
	bi.bV5AlphaMask = 0xFF000000;

	void* bits = nullptr;
	HDC screenDc = GetDC(nullptr);
	HBITMAP dib = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
	HDC memDc = CreateCompatibleDC(screenDc);
	ReleaseDC(nullptr, screenDc);
	if (!dib || !memDc || !bits) {
		if (memDc) {
			DeleteDC(memDc);
		}
		if (dib) {
			DeleteObject(dib);
		}
		return nullptr;
	}

	HGDIOBJ oldBitmap = SelectObject(memDc, dib);
	PatBlt(memDc, 0, 0, size, size, BLACKNESS);
	DrawIconEx(memDc, 0, 0, hIcon, size, size, 0, nullptr, DI_NORMAL);

	D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
		D2D1_BITMAP_OPTIONS_NONE,
		D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
		96.0f,
		96.0f);

	ComPtr<ID2D1Bitmap1> bitmap;
	d2dContext->CreateBitmap(D2D1::SizeU(size, size), bits, size * 4, &bitmapProperties, &bitmap);

	SelectObject(memDc, oldBitmap);
	DeleteDC(memDc);
	DeleteObject(dib);
	return bitmap;
}

// =========================================================
// 5. UIAnimation 硬件定时器引擎
// =========================================================
ComPtr<IUIAnimationManager> g_AnimManager;
ComPtr<IUIAnimationTimer> g_AnimTimer;
ComPtr<IUIAnimationTransitionLibrary> g_AnimLibrary;
ComPtr<IUIAnimationVariable> g_AnimProgressVar;
ComPtr<IUIAnimationVariable> g_VarMenuAlpha; // 0.0: Title visible, 1.0: Menu visible
ComPtr<IUIAnimationVariable> g_VarHoverAlpha[4];

void PlayAnimation(ComPtr<IUIAnimationVariable>& var, double target, double duration) {
	if (!g_AnimManager || !var) return;
	double current = 0; var->GetValue(&current);
	if (current == target) return;
	ComPtr<IUIAnimationStoryboard> sb; g_AnimManager->CreateStoryboard(&sb);
	ComPtr<IUIAnimationTransition> trans;
	g_AnimLibrary->CreateAccelerateDecelerateTransition(duration, target, 0.3, 0.3, &trans);
	sb->AddTransition(var.Get(), trans.Get());
	UI_ANIMATION_SECONDS now; g_AnimTimer->GetTime(&now); sb->Schedule(now);

}

void StartProgressPingPongAnimation() {
	if (!g_AnimManager || !g_AnimProgressVar) return;
	double currentValue = 0; g_AnimProgressVar->GetValue(&currentValue);
	PlayAnimation(g_AnimProgressVar, (currentValue < 50.0) ? 100.0 : 0.0, 2.0);
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

		double progressVal = 0; g_AnimProgressVar->GetValue(&progressVal);
		PostMessage(g_hPanel, WM_UPDATE_PROGRESS, (WPARAM)progressVal, 0);
		double menuA = 0; g_VarMenuAlpha->GetValue(&menuA);
		std::array<float, 4> hoverA = { 0 };
		for (int i = 0; i < g_MenuCount; ++i) {
			double val = 0; g_VarHoverAlpha[i]->GetValue(&val);
			hoverA[i] = static_cast<float>(val);
		}
		g_CommandQueue.push(CmdUpdateAnim{ static_cast<float>(menuA), static_cast<float>(progressVal) / 100.0f, hoverA });
		g_RenderEvent.Notify();

		UI_ANIMATION_MANAGER_STATUS status; g_AnimManager->GetStatus(&status);
		if (status == UI_ANIMATION_MANAGER_IDLE) StartProgressPingPongAnimation(); return S_OK;
	}
	IFACEMETHODIMP OnRenderingTooSlow(UINT32 framesPerSecond) override { return S_OK; }
};

void InitUIAnimation() {
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	CoCreateInstance(CLSID_UIAnimationManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_AnimManager));
	CoCreateInstance(CLSID_UIAnimationTimer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_AnimTimer));
	CoCreateInstance(CLSID_UIAnimationTransitionLibrary, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_AnimLibrary));
	g_AnimManager->CreateAnimationVariable(0.0, &g_AnimProgressVar);
	g_AnimManager->CreateAnimationVariable(0.0, &g_VarMenuAlpha);
	for (int i = 0; i < g_MenuCount; ++i) g_AnimManager->CreateAnimationVariable(0.0, &g_VarHoverAlpha[i]);
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
// 8. GPU 渲染线程（重构为 CRTP + RAII）
// =========================================================

template <typename Derived>
class RenderThreadScope {
public:
	RenderThreadScope() {
		InitializeThread();
		InitializeDevicesAndFactories();
		InitializeWriteFormats();
		InitializeCompositionAndSwapChain();
		InitializeBrushes();

		guiBridge = std::make_unique<ImGuiBridgeImpl>();
		guiBridge->Init(g_hCanvas, d3dDevice.Get(), d3dContext.Get());

		CreateTarget();

		showGrid = true; aimRadius = 18.0f; aimStyle = 0;
		hitMarks.clear();
		currentMenuAlpha = 0.0f; currentHoverAlphas = { 0 };

		g_GpuInitDoneEvent.Notify();
	}

	~RenderThreadScope() {
		if (m_hAvrt) {
			AvRevertMmThreadCharacteristics(m_hAvrt);
			m_hAvrt = nullptr;
		}
		if (guiBridge) {
			guiBridge->Shutdown();
			guiBridge.reset();
		}
		if (d2dContext) d2dContext->SetTarget(nullptr);
	}

	void Run() {
		EnsureUiHost();
		RunLoop();
	}

private:
	void EnsureUiHost() {
		if (uiHost || !d2dFactory || !d2dDevice || !d2dContext || !dwriteFactory) {
			return;
		}
		uiHost = std::make_unique<fusion::ui::DemoUiHost>(
			fusion::ui::GraphicsContext{ d2dFactory, d2dDevice, d2dContext, dwriteFactory },
			static_cast<Derived*>(this)->CreateUiCallbacks());
		utils::console("ui host created\n");
	}

	void InitializeThread() {
		DWORD_PTR pCoreMask = GetPCoreMask();
		if (pCoreMask) SetThreadAffinityMask(GetCurrentThread(), pCoreMask);

		DWORD taskIndex = 0;
		m_hAvrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
		if (m_hAvrt) AvSetMmThreadPriority(m_hAvrt, AVRT_PRIORITY_CRITICAL);
	}

	void InitializeDevicesAndFactories() {
		D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext);
		d3dDevice.As(&dxgiDevice); if (dxgiDevice) dxgiDevice->SetMaximumFrameLatency(1);

		D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &d2dFactory);
		if (d2dFactory) d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
		if (d2dDevice) d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext);

		DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
	}

	void InitializeWriteFormats() {
		titleFormat = CreateCaptionTextFormat(dwriteFactory.Get(), g_hMainWindow);
		if (!titleFormat) {
			dwriteFactory->CreateTextFormat(L"Microsoft YaHei UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-cn", &titleFormat);
			if (titleFormat) { titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
		}
		dwriteFactory->CreateTextFormat(L"Microsoft YaHei UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-cn", &menuFormat);
		if (menuFormat) { menuFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); menuFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
	}

	void InitializeCompositionAndSwapChain() {
		dxgiDevice->GetAdapter(&dxgiAdapter);
		dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), &dxgiFactory);

		RECT rc; GetClientRect(g_hCanvas, &rc);
		currentWidth = max(rc.right - rc.left, 1);
		currentHeight = max(rc.bottom - rc.top, 1);

		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.Width = currentWidth; swapChainDesc.Height = currentHeight;
		swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; swapChainDesc.SampleDesc.Count = 1;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; swapChainDesc.BufferCount = 2;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

		dxgiFactory->CreateSwapChainForComposition(d3dDevice.Get(), &swapChainDesc, nullptr, &swapChain);

		DCompositionCreateDevice2(d2dDevice.Get(), IID_PPV_ARGS(&dcompDevice));
		dcompDevice->CreateTargetForHwnd(g_hCanvas, TRUE, &dcompTarget);

		dcompDevice->CreateVisual(&rootVisual);
		dcompDevice->CreateVisual(&mainVisual);
		mainVisual->SetContent(swapChain.Get());
		rootVisual->AddVisual(mainVisual.Get(), FALSE, nullptr);
		dcompDevice->CreateVisual(&titleVisual);
		dcompDevice->CreateVirtualSurface(currentWidth, max(64, g_CaptionHeight.load()), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &titleSurface);
		titleVisual->SetContent(titleSurface.Get());
		rootVisual->AddVisual(titleVisual.Get(), TRUE, mainVisual.Get());
		dcompDevice->CreateVisual(&uiVisual);
		dcompDevice->CreateVirtualSurface(currentWidth, currentHeight, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &uiSurface);
		uiVisual->SetContent(uiSurface.Get());
		rootVisual->AddVisual(uiVisual.Get(), TRUE, titleVisual.Get());
		dcompTarget->SetRoot(rootVisual.Get()); dcompDevice->Commit();
	}

	void InitializeBrushes() {
		if (d2dContext) {
			d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.f, 0.f, 0.f, 1.0f), &brushPaint);
			d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.98f, 1.0f), &brushBackground);
			d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.3f), &brushGrid);
			d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.6f, 0.2f), &brushAim);
			d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.2f, 0.0f), &brushHit);
			d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f), &brushMenuText);
			d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f), &brushTitleText);
		}
	}

	void CreateTarget() {
		if (!swapChain || !d2dContext) return;
		ComPtr<IDXGISurface> dxgiBackBuffer; swapChain->GetBuffer(0, __uuidof(IDXGISurface), &dxgiBackBuffer);
		D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		d2dContext->CreateBitmapFromDxgiSurface(dxgiBackBuffer.Get(), &bp, &d2dTargetBitmap);
		d2dContext->SetTarget(d2dTargetBitmap.Get());
		if (guiBridge) guiBridge->ResizeOffscreenTarget(d3dDevice.Get(), d2dContext.Get(), currentWidth, currentHeight);
	}

	void RunLoop() {
		LONG currentRenderSignal = g_RenderEvent.Capture();

		while (g_isRunning.load(std::memory_order_relaxed)) {
			bool queueHasCmds = !g_CommandQueue.is_empty();
			bool isAnimating = (currentMenuAlpha > 0.01f && currentMenuAlpha < 0.99f);
			for (int i = 0; i < g_MenuCount; ++i) { if (currentHoverAlphas[i] > 0.01f && currentHoverAlphas[i] < 0.99f) isAnimating = true; }

			if (!queueHasCmds) {
				if (isAnimating) g_RenderEvent.HybridWait(currentRenderSignal, 500, 16);
				else g_RenderEvent.HybridWait(currentRenderSignal, 1500, INFINITE);
				LONG newRenderSignal = g_RenderEvent.Capture();
				if (newRenderSignal == currentRenderSignal && !isAnimating) continue;
				currentRenderSignal = newRenderSignal;
			}

			if (!g_isRunning.load(std::memory_order_relaxed)) break;

			auto begin = std::chrono::steady_clock::now();
			bool receivedResizeCmd = false; // [新增]

			int pendingResizeW = -1, pendingResizeH = -1;

			RenderCommand cmd;
			while (g_CommandQueue.pop(cmd)) {
				HandleCommand(cmd, pendingResizeW, pendingResizeH);
			}

			if (pendingResizeW > 0 && pendingResizeH > 0) {
				receivedResizeCmd = true; // [新增] 标记我们收到了 Resize 指令
				if (pendingResizeW != currentWidth || pendingResizeH != currentHeight) {
					d2dContext->SetTarget(nullptr); d2dTargetBitmap.Reset();
					d2dContext->Flush(); ComPtr<ID3D11DeviceContext> d3dContext2; d3dDevice->GetImmediateContext(&d3dContext2);
					d3dContext2->ClearState(); d3dContext2->Flush();
					if (SUCCEEDED(swapChain->ResizeBuffers(2, pendingResizeW, pendingResizeH, DXGI_FORMAT_UNKNOWN, 0))) {
						currentWidth = pendingResizeW;
					currentHeight = pendingResizeH;
					if (titleSurface) {
						titleSurface->Resize(pendingResizeW, max(64, g_CaptionHeight.load()));
					}
						if (uiSurface) {
							uiSurface->Resize(pendingResizeW, pendingResizeH);
						}
					CreateTarget();
					}
				}
			}
			if (!g_isRunning.load(std::memory_order_relaxed)) break;

			guiBridge->RenderFrame(d3dContext.Get(), currentMenuAlpha, showGrid);

			d2dContext->SetTarget(d2dTargetBitmap.Get());
			d2dContext->BeginDraw();
			d2dContext->SetDpi(96.0f * g_dpiScale, 96.0f * g_dpiScale);

			d2dContext->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

			float logicalW = currentWidth / g_dpiScale; float logicalH = currentHeight / g_dpiScale;
			float captionHeightLogical = g_CaptionHeight.load() / g_dpiScale;
			float panelLogicalW = (float)GetPanelWidth() / g_dpiScale;
			D2D1_RECT_F contentRect = D2D1::RectF(panelLogicalW, captionHeightLogical, logicalW, logicalH);
			d2dContext->FillRectangle(contentRect, brushBackground.Get());

			// 委托给派生类完成具体绘制
			static_cast<Derived*>(this)->OnRender(logicalW, logicalH, captionHeightLogical, panelLogicalW);

			d2dContext->EndDraw();

			if (uiHost && uiSurface) {
				ComPtr<ID2D1DeviceContext> d2dContextForUi;
				POINT uiOffset{};
				RECT uiUpdateRect = { 0, 0, currentWidth, currentHeight };
				if (SUCCEEDED(uiSurface->BeginDraw(&uiUpdateRect, IID_PPV_ARGS(&d2dContextForUi), &uiOffset))) {
					d2dContextForUi->SetDpi(96.0f * g_dpiScale, 96.0f * g_dpiScale);
					d2dContextForUi->SetTransform(D2D1::Matrix3x2F::Translation(uiOffset.x / g_dpiScale, uiOffset.y / g_dpiScale));
					d2dContextForUi->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
					uiHost->SetViewport(contentRect, g_dpiScale);
					uiHost->Render(d2dContextForUi.Get());
					const auto uiBounds = uiHost->VisibleUiBounds();
					g_UiVisualLeft.store(static_cast<int>(uiBounds.left * g_dpiScale), std::memory_order_relaxed);
					g_UiVisualTop.store(static_cast<int>(uiBounds.top * g_dpiScale), std::memory_order_relaxed);
					g_UiVisualRight.store(static_cast<int>(uiBounds.right * g_dpiScale), std::memory_order_relaxed);
					g_UiVisualBottom.store(static_cast<int>(uiBounds.bottom * g_dpiScale), std::memory_order_relaxed);
					switch (uiHost->CurrentCursor()) {
					case fusion::ui::CursorKind::Hand:
						g_UiCursorState.store(static_cast<int>(UiCursorState::Hand), std::memory_order_relaxed);
						break;
					case fusion::ui::CursorKind::IBeam:
						g_UiCursorState.store(static_cast<int>(UiCursorState::IBeam), std::memory_order_relaxed);
						break;
					case fusion::ui::CursorKind::Arrow:
						g_UiCursorState.store(static_cast<int>(UiCursorState::Arrow), std::memory_order_relaxed);
						break;
					default:
						g_UiCursorState.store(static_cast<int>(UiCursorState::CanvasHidden), std::memory_order_relaxed);
						break;
					}
					uiSurface->EndDraw();
				}
			}

			if (titleSurface) {
				ComPtr<ID2D1DeviceContext> d2dContextForTitle;
				POINT offset{};
				int captionH = max(1, g_CaptionHeight.load());
				RECT updateRect = { 0, 0, currentWidth, captionH };

				// 听你的！直接拿 DComp 配置好 Clip 和 Target 的完美 Context，绝不撕裂！
				if (SUCCEEDED(titleSurface->BeginDraw(&updateRect, IID_PPV_ARGS(&d2dContextForTitle), &offset))) {

					// DComp 已经在内部把原点(0,0)对齐到了这块图集区域，无脑 SetDpi 即可
					d2dContextForTitle->SetDpi(96.0f * g_dpiScale, 96.0f * g_dpiScale);
					
					// 【重要修复】：DComp 返回的 offset 是物理像素，在图集(Atlas)上的偏移量
					// 必须向 D2D 提供基于逻辑像素(DIP)的 Transform 偏移，不然就是满天乱飞的闪烁
					d2dContextForTitle->SetTransform(D2D1::Matrix3x2F::Translation(offset.x / g_dpiScale, offset.y / g_dpiScale));

					// 官方配发的 Context 处于完美的隔离态，且由于 DComp 初始化了 Clip，可以放心 Clear
					d2dContextForTitle->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

					// 临时借尸还魂：因为画刷都是同一个 d2dDevice 创建的，完美互通！
					auto originalContext = d2dContext;
					d2dContext = d2dContextForTitle;

					// 绘制标题与菜单图层
					static_cast<Derived*>(this)->OnRenderTitleLayer(logicalW, captionHeightLogical);

					// 用完还回去
					d2dContext = originalContext;

					titleSurface->EndDraw();
				}
			}

			// 2. 提交 DComp 变更并统一进行 SwapChain 刷新，保证同帧同步
			dcompDevice->Commit();
			swapChain->Present(0, 0);

			if (receivedResizeCmd) {
				// [新增] 一旦完成重绘并 Present，立刻唤醒阻塞中的 UI 线程完成 NCCALCSIZE
				g_ResizeDoneEvent.Notify();
			}

			auto pass = std::chrono::duration_cast<std::chrono::milliseconds>((std::chrono::steady_clock::now() - begin)).count();
			utils::console("\ndraw frame in {} ms\n\n", pass);
		}
	}

	void HandleCommand(RenderCommand& cmd, int& pendingResizeW, int& pendingResizeH) {
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
			else if constexpr (std::is_same_v<T, CmdUpdateAnim>) {
				currentMenuAlpha = arg.menuAlpha; currentHoverAlphas = arg.hoverAlpha;
				if (uiHost) {
					uiHost->SetAnimationProgress(arg.progress);
				}
			}
			else if constexpr (std::is_same_v<T, CmdKeyDown>) {
				const bool consumedByUi = uiHost && uiHost->HandleKeyDown(arg.key, fusion::ui::KeyModifiers{ arg.ctrl, arg.shift, arg.alt });
				if (consumedByUi) {
					return;
				}
				if ((arg.key == 'C' || arg.key == 'c') && !arg.ctrl) hitMarks.clear();
				if (arg.key == VK_SPACE) {
					POINT pt; GetCursorPos(&pt); ScreenToClient(g_hCanvas, &pt);
					if (pt.x > (int)(200 * g_dpiScale)) hitMarks.push_back({ (float)pt.x / g_dpiScale, (float)pt.y / g_dpiScale });
				}
			}
			else if constexpr (std::is_same_v<T, CmdWin32Msg>) {
				guiBridge->HandleWin32Message(arg.hwnd, arg.msg, arg.wParam, arg.lParam);
				if (uiHost) {
					uiHost->HandleWin32Message(arg.hwnd, arg.msg, arg.wParam, arg.lParam);
				}
			}
			else if constexpr (std::is_same_v<T, CmdExit>) g_isRunning.store(false, std::memory_order_relaxed);
			else if constexpr (std::is_same_v<T, CmdUpdateTitleBar>) {
				cachedWindowTitle = arg.title.data();
				cachedButtonBounds = arg.buttonRect;
				// 创建并缓存 D2D Bitmap，避免每帧重复创建引发性能灾难
				if (arg.hIcon && d2dContext) {
					cachedIconBitmap = CreateBitmapFromHicon(d2dContext.Get(), arg.hIcon, arg.iconSizePx);
					cachedIconSizePx = arg.iconSizePx;
				}
			}
			}, cmd);
	}

protected:
	HANDLE m_hAvrt = nullptr;
	ComPtr<ID3D11Device> d3dDevice; 
	ComPtr<ID3D11DeviceContext> d3dContext; 
	
	ComPtr<IDXGIDevice1> dxgiDevice;
	ComPtr<IDXGIAdapter> dxgiAdapter;
	ComPtr<IDXGIFactory2> dxgiFactory;

	ComPtr<ID2D1Factory1> d2dFactory; 
	ComPtr<ID2D1Device> d2dDevice; 
	ComPtr<ID2D1DeviceContext> d2dContext;
	
	ComPtr<IDWriteFactory> dwriteFactory; 
	ComPtr<IDWriteTextFormat> titleFormat, menuFormat;
	 
	ComPtr<IDXGISwapChain1> swapChain;

	ComPtr<IDCompositionDesktopDevice> dcompDevice; 
	ComPtr<IDCompositionTarget> dcompTarget; 
	ComPtr<IDCompositionVisual2> rootVisual, mainVisual, titleVisual, uiVisual;
	ComPtr<IDCompositionVirtualSurface> titleSurface;
	ComPtr<IDCompositionVirtualSurface> uiSurface;

	ComPtr<ID2D1Bitmap1> d2dTargetBitmap;
	ComPtr<ID2D1SolidColorBrush> brushGrid, brushAim, brushHit, brushMenuText, brushBackground, brushPaint, brushTitleText;
	std::unique_ptr<ImGuiBridge> guiBridge;
	std::unique_ptr<fusion::ui::DemoUiHost> uiHost;

	int currentWidth = 0; int currentHeight = 0;
	bool showGrid = true; float aimRadius = 18.0f; int aimStyle = 0;
	std::vector<D2D1_POINT_2F> hitMarks;
	float currentMenuAlpha = 0.0f; std::array<float, 4> currentHoverAlphas = { 0 };
	std::wstring cachedWindowTitle;
	RECT cachedButtonBounds{ 0, 0, 0, 0 };
	ComPtr<ID2D1Bitmap1> cachedIconBitmap;
	int cachedIconSizePx = 0;
};

// 默认绘制实现，保留原本绘制逻辑
class FusionRenderer : public RenderThreadScope<FusionRenderer> {
public:
	fusion::ui::HostCallbacks CreateUiCallbacks() {
		return fusion::ui::HostCallbacks{
			[this](bool show) { showGrid = show; },
			[this](int style) { aimStyle = style; },
			[this](float radius) { aimRadius = radius; },
			[this]() { hitMarks.clear(); }
		};
	}

	void OnRender(float logicalW, float logicalH, float captionHeightLogical, float panelLogicalW) {
		// grid
		if (showGrid) {
			for (float x = panelLogicalW; x < logicalW; x += 40.0f)
				d2dContext->DrawLine(D2D1::Point2F(x, captionHeightLogical), D2D1::Point2F(x, logicalH), brushGrid.Get());
			for (float y = captionHeightLogical; y < logicalH; y += 40.0f)
				d2dContext->DrawLine(D2D1::Point2F(panelLogicalW, y), D2D1::Point2F(logicalW, y), brushGrid.Get());
		}

		for (const auto& hit : hitMarks) {
			d2dContext->DrawLine(D2D1::Point2F(hit.x - 6, hit.y - 6), D2D1::Point2F(hit.x + 6, hit.y + 6), brushHit.Get(), 3.0f);
			d2dContext->DrawLine(D2D1::Point2F(hit.x - 6, hit.y + 6), D2D1::Point2F(hit.x + 6, hit.y - 6), brushHit.Get(), 3.0f);
		}

		POINT pt; GetCursorPos(&pt); ScreenToClient(g_hCanvas, &pt);
		if (g_bMouseInCanvas.load(std::memory_order_relaxed) && pt.x > (int)(200 * g_dpiScale) && pt.y > g_CaptionHeight.load() && !g_ImGuiWantCaptureMouse.load(std::memory_order_relaxed)) {
			float fx = (float)pt.x / g_dpiScale, fy = (float)pt.y / g_dpiScale;
			if (aimStyle == 0) {
				d2dContext->DrawLine(D2D1::Point2F(fx - aimRadius - 7, fy), D2D1::Point2F(fx + aimRadius + 7, fy), brushAim.Get(), 2.0f);
				d2dContext->DrawLine(D2D1::Point2F(fx, fy - aimRadius - 7), D2D1::Point2F(fx, fy + aimRadius + 7), brushAim.Get(), 2.0f);
				d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(fx, fy), aimRadius, aimRadius), brushAim.Get(), 2.0f);
			}
			else if (aimStyle == 1) {
				d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fx, fy), aimRadius * 0.3f, aimRadius * 0.3f), brushAim.Get());
			}
			else if (aimStyle == 2) {
				d2dContext->DrawLine(D2D1::Point2F(fx, fy - aimRadius), D2D1::Point2F(fx - aimRadius, fy + aimRadius), brushAim.Get(), 2.0f);
				d2dContext->DrawLine(D2D1::Point2F(fx - aimRadius, fy + aimRadius), D2D1::Point2F(fx + aimRadius, fy + aimRadius), brushAim.Get(), 2.0f);
				d2dContext->DrawLine(D2D1::Point2F(fx + aimRadius, fy + aimRadius), D2D1::Point2F(fx, fy - aimRadius), brushAim.Get(), 2.0f);
				d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(fx, fy), 2.0f, 2.0f), brushAim.Get(), 2.0f);
			}
		}
	}

	void OnRenderTitleLayer(float logicalW, float captionHeightLogical) {
		drawTitleAndCaptionButtons(1.0f - currentMenuAlpha, captionHeightLogical, logicalW);
		int pressedIdx = g_PressedMenuIndexForRender.load(std::memory_order_acquire);
		if (currentMenuAlpha > 0.01f) {
			float startX = GetMenuStartLogicalX();
			for (int i = 0; i < g_MenuCount; ++i) {
				float itemWidth = g_MenuItems[i].width;
				float hAlpha = currentHoverAlphas[i];
				bool isPressed = (i == pressedIdx);

				if (hAlpha > 0.01f || isPressed) {
					float bgAlpha = isPressed ? 1.f : hAlpha;
					brushPaint->SetColor(D2D1::ColorF(0.f, 0.f, 0.f, 0.1f * currentMenuAlpha * bgAlpha));
					d2dContext->FillRectangle(D2D1::RectF(startX, 0, startX + itemWidth, captionHeightLogical), brushPaint.Get());
				}

				brushMenuText->SetOpacity(currentMenuAlpha);
				d2dContext->DrawTextW(g_MenuItems[i].text, static_cast<UINT32>(wcslen(g_MenuItems[i].text)), menuFormat.Get(),
					D2D1::RectF(startX + g_MenuPadding / 2.0f, 0.0f, startX + itemWidth, captionHeightLogical), brushMenuText.Get());
				startX += itemWidth;
			}
		}
	}

private:
	void drawTitleAndCaptionButtons(float titleAlpha, float captionHeightLogical, float logicalW) {

		if (titleAlpha <= 0.01f) return;

		UINT dpi = GetDpiForWindow(g_hMainWindow);
		int iconSizePx = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
		float iconSizeLogical = (float)iconSizePx / g_dpiScale;
		float startX = GetMenuStartLogicalX();
		float iconTop = (captionHeightLogical - iconSizeLogical) * 0.5f;

		// 1. 直接使用缓存好的 Bitmap 绘制图标
		if (cachedIconBitmap) {
			d2dContext->DrawBitmap(cachedIconBitmap.Get(), D2D1::RectF(startX, iconTop, startX + iconSizeLogical, iconTop + iconSizeLogical), titleAlpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
		}

		// 2. 直接使用缓存好的边界和标题绘制文字
		float textRight = logicalW;
		if (!IsRectEmpty(&cachedButtonBounds)) {
			textRight = min(textRight, (float)cachedButtonBounds.left / g_dpiScale - kCaptionLogicalInset);
		}

		brushTitleText->SetOpacity(titleAlpha);
		float textLeft = startX + iconSizeLogical + kCaptionTextGap;
		if (textRight > textLeft && !cachedWindowTitle.empty()) {
			d2dContext->DrawTextW(cachedWindowTitle.c_str(), static_cast<UINT32>(cachedWindowTitle.length()), titleFormat.Get(), D2D1::RectF(textLeft, 0.0f, textRight, captionHeightLogical), brushTitleText.Get());
		}
	}
};

void RenderThreadFunc() {
	FusionRenderer r;
	r.Run();
}


void PushTitleBarInfoToGPU(HWND hwnd) {
	CmdUpdateTitleBar cmd{};

	// 获取图标
	cmd.hIcon = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0);
	if (!cmd.hIcon) cmd.hIcon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
	if (!cmd.hIcon) cmd.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

	// 获取文字
	cmd.title.fill(0);
	GetWindowTextW(hwnd, cmd.title.data(), 255);

	// 获取热区
	DwmGetWindowAttribute(hwnd, DWMWA_CAPTION_BUTTON_BOUNDS, &cmd.buttonRect, sizeof(cmd.buttonRect));

	// 获取 DPI 图标尺寸
	cmd.iconSizePx = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(hwnd));

	g_CommandQueue.push(cmd);
}

// =========================================================
// UI 线程：原生 Win32 右键菜单生成器 
// =========================================================
LRESULT CALLBACK MenuMsgFilterHook(int code, WPARAM wParam, LPARAM lParam) {
	if (code == MSGF_MENU) {
		MSG* msg = (MSG*)lParam;
		if (msg->message == WM_MOUSEMOVE) {
			POINT pt = msg->pt;
			ScreenToClient(g_hMainWindow, &pt);
			if (IsInMenuBarBand(pt)) {
				int hoverIdx = GetMenuIndexFromLogicalX((float)pt.x / g_dpiScale);
				if (hoverIdx != -1 && hoverIdx != g_PressedMenuIndexForRender.load()) {
					g_NextMenuIndex.store(hoverIdx);
					PostMessageW(g_hMainWindow, WM_CANCELMODE, 0, 0);
				}
			}
		}
	}
	return CallNextHookEx(NULL, code, wParam, lParam);
}

void ShowCustomWin32Menu(HWND hwnd, int selectedIndex) {
	int currentIndex = selectedIndex;

	while (currentIndex >= 0 && currentIndex < g_MenuCount) {
		g_NextMenuIndex.store(-1);
		g_PressedMenuIndexForRender.store(currentIndex, std::memory_order_release);

		HMENU hMenu = CreatePopupMenu();
		if (currentIndex == 0) {
			AppendMenuW(hMenu, MF_STRING, 3001, L"新建画布 (New)\tCtrl+N");
			HMENU hRecent = CreatePopupMenu();
			AppendMenuW(hRecent, MF_STRING, 3002, L"项目: 赛博空间");
			AppendMenuW(hRecent, MF_STRING, 3003, L"项目: 极简打靶场");
			AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hRecent, L"打开近期 (Open Recent) ►");
			AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
			AppendMenuW(hMenu, MF_STRING, 2002, L"退出 (Exit)\tAlt+F4");
		}
		else if (currentIndex == 1) {
			AppendMenuW(hMenu, MF_STRING, 2001, L"撤销弹孔 (Undo)\tCtrl+Z");
			AppendMenuW(hMenu, MF_STRING, 3004, L"清空所有 (Clear All)\tC");
		}
		else if (currentIndex == 2) {
			AppendMenuW(hMenu, MF_STRING | MF_CHECKED, 3005, L"显示网格 (Show Grid)");
			AppendMenuW(hMenu, MF_STRING, 3006, L"性能监控面板 (Perf Overlay)");
		}
		else if (currentIndex == 3) {
			AppendMenuW(hMenu, MF_STRING, 3007, L"关于 D2D 引擎 (About)...");
		}

		float popupX = GetMenuStartLogicalX();
		for (int i = 0; i < currentIndex; ++i) popupX += g_MenuItems[i].width;

		POINT pt = { (int)(popupX * g_dpiScale), g_CaptionHeight.load() };
		ClientToScreen(hwnd, &pt);
		g_IsMenuPopupActive.store(true, std::memory_order_relaxed);
		if (!g_MenuVisibleState) { g_MenuVisibleState = true; PlayAnimation(g_VarMenuAlpha, 1.0, 0.2); }
		g_RenderEvent.Notify();
		SetForegroundWindow(hwnd);
		HHOOK hHook = SetWindowsHookExW(WH_MSGFILTER, MenuMsgFilterHook, NULL, GetCurrentThreadId());
		UINT command = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, g_hMainWindow, NULL);
		UnhookWindowsHookEx(hHook);
		if (command != 0) {
			SendMessageW(g_hMainWindow, WM_COMMAND, MAKEWPARAM(command, 0), 0);
		}
		DestroyMenu(hMenu);

		currentIndex = g_NextMenuIndex.load();
	}

	g_PressedMenuIndexForRender.store(-1, std::memory_order_release);
	g_IsMenuPopupActive.store(false, std::memory_order_relaxed);
	UpdateTitleMenuVisibilityFromCursor(hwnd);
	g_RenderEvent.Notify();
}

// =========================================================
// UI 线程：顶层 Panel (完全自适应 DPI)
// =========================================================
LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_CREATE: {
		int y = GetPanelContentTop();
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

		InitUIAnimation();
		StartProgressPingPongAnimation();
		return 0;
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
	case WM_NCHITTEST: {
		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hwnd, &pt);
		if (pt.y <= g_CaptionHeight.load()) return HTTRANSPARENT;
		break;
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
	static auto init = 0;
	switch (msg) {

	case WM_CREATE: {
		g_dpiScale = GetDpiForWindow(hwnd) / 96.0f;
		g_CaptionHeight.store(compute_standard_caption_height_for_window(hwnd), std::memory_order_relaxed);
		SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
		//SetWindowLongPtr(hwnd, GWL_EXSTYLE, GetWindowLongPtr(hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
		ExtendFrameIntoClient(hwnd);
		PushTitleBarInfoToGPU(hwnd); // [新增]
		ImmAssociateContext(hwnd, NULL);
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);

		RECT rcClient;
		GetClientRect(hwnd, &rcClient);
		return 0;
	}
	case WM_ERASEBKGND: {
		return 1;
	}
	case WM_ACTIVATE: {
		ExtendFrameIntoClient(hwnd);
		return 0;
	}
	case WM_NCCALCSIZE: {
		if (wParam == TRUE) {
			NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
			//if (IsZoomed(hwnd)) 
			{
				UINT dpi = GetDpiForWindow(hwnd);
				int frameX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
				int frameY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
				params->rgrc[0].left += frameX;
				params->rgrc[0].right -= frameX;
				//params->rgrc[0].top += frameY;
				params->rgrc[0].bottom -= frameY;

			}

			// [新增] 计算新的客户区宽高
			int newWidth = params->rgrc[0].right - params->rgrc[0].left;
			int newHeight = params->rgrc[0].bottom - params->rgrc[0].top;

			// [新增] 拦截缩放，通知渲染线程并阻塞等待新帧上屏
			static int s_lastWidth = 0, s_lastHeight = 0;
			if (newWidth > 0 && newHeight > 0 && (newWidth != s_lastWidth || newHeight != s_lastHeight)) {
				s_lastWidth = newWidth;
				s_lastHeight = newHeight;

				if (g_RenderThreadReady.load(std::memory_order_acquire)) {
					PushTitleBarInfoToGPU(hwnd);
					LONG expected = g_ResizeDoneEvent.Capture();
					g_CommandQueue.push(CmdResize{ newWidth, newHeight });
					g_RenderEvent.Notify();
					// UI线程阻塞，直到渲染线程完成本次 Resize 并 Present
					g_ResizeDoneEvent.HybridWait(expected, 1500, INFINITE);
				}
			}

			return 0;
		}
		break;
	}

	case WM_NCHITTEST: {
		LRESULT hit;
		if (DwmDefWindowProc(hwnd, msg, wParam, lParam, &hit)) return hit;

		RECT windowRect{};
		GetWindowRect(hwnd, &windowRect);
		POINT screenPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (!IsZoomed(hwnd))
		{
			const int resizeBorder = GetSystemMetricsForDpi(SM_CXSIZEFRAME, GetDpiForWindow(hwnd)) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(hwnd));

			if (screenPt.y < windowRect.top + resizeBorder && screenPt.x < windowRect.left + resizeBorder) return HTTOPLEFT;
			if (screenPt.y < windowRect.top + resizeBorder && screenPt.x >= windowRect.right - resizeBorder) return HTTOPRIGHT;
			if (screenPt.y >= windowRect.bottom - resizeBorder && screenPt.x < windowRect.left + resizeBorder) return HTBOTTOMLEFT;
			if (screenPt.y >= windowRect.bottom - resizeBorder && screenPt.x >= windowRect.right - resizeBorder) return HTBOTTOMRIGHT;
			if (screenPt.y < windowRect.top + resizeBorder) return HTTOP;
			if (screenPt.y >= windowRect.bottom - resizeBorder) return HTBOTTOM;
			if (screenPt.x < windowRect.left + resizeBorder) return HTLEFT;
			if (screenPt.x >= windowRect.right - resizeBorder) return HTRIGHT;
		}

		POINT clientPt = screenPt;
		ScreenToClient(hwnd, &clientPt);
		RECT buttonRect = GetCaptionButtonBounds(hwnd);
		if (!IsRectEmpty(&buttonRect) && PtInRect(&buttonRect, clientPt)) {
			int buttonWidth = max((buttonRect.right - buttonRect.left) / 3, 1);
			int buttonIndex = min(2, max(0, (clientPt.x - buttonRect.left) / buttonWidth));
			if (buttonIndex == 0) return HTMINBUTTON;
			if (buttonIndex == 1) return HTMAXBUTTON;
			return HTCLOSE;
		}
		if (IsInMenuBarBand(clientPt)) {
			return HTCLIENT;
		}
		if (IsInDraggableCaptionBand(clientPt)) {
			return HTCAPTION;
		}
		return HTCLIENT;
	}

	case WM_DPICHANGED: {
		g_dpiScale = HIWORD(wParam) / 96.0f;
		g_CaptionHeight.store(compute_standard_caption_height_for_window(hwnd), std::memory_order_relaxed);
		UpdateGlobalFont(); // 刷新字体大小
		PushTitleBarInfoToGPU(hwnd); // [新增]
		RECT* const prcNewWindow = (RECT*)lParam;
		SetWindowPos(hwnd, NULL, prcNewWindow->left, prcNewWindow->top, prcNewWindow->right - prcNewWindow->left, prcNewWindow->bottom - prcNewWindow->top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		ExtendFrameIntoClient(hwnd);
		return 0;
	}
	case WM_COMMAND: {
		if (LOWORD(wParam) == 3004 || LOWORD(wParam) == 2001) { g_CommandQueue.push(CmdResetCanvas{}); g_RenderEvent.Notify(); }
		if (LOWORD(wParam) == 2002) { DestroyWindow(hwnd); }
		return 0;
	}
	case WM_MOUSEMOVE: {
		if (!g_bMouseInCanvas.load(std::memory_order_relaxed)) {
			TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tme);
			g_bMouseInCanvas.store(true, std::memory_order_relaxed);
		}

		int y = GET_Y_LPARAM(lParam); int x = GET_X_LPARAM(lParam); POINT pt = { x, y };
		bool shouldShowMenu = g_IsMenuPopupActive.load() || IsInCustomCaptionBand(pt);
		if (shouldShowMenu != g_MenuVisibleState) {
			g_MenuVisibleState = shouldShowMenu;
			PlayAnimation(g_VarMenuAlpha, shouldShowMenu ? 1.0 : 0.0, 0.2);
		}
		int hoverIdx = (shouldShowMenu && IsInMenuBarBand(pt)) ? GetMenuIndexFromLogicalX((float)x / g_dpiScale) : -1;
		if (hoverIdx != g_HoveredMenuIndex) {
			if (g_HoveredMenuIndex != -1) PlayAnimation(g_VarHoverAlpha[g_HoveredMenuIndex], 0.0, 0.15);
			if (hoverIdx != -1) PlayAnimation(g_VarHoverAlpha[hoverIdx], 1.0, 0.15);
			g_HoveredMenuIndex = hoverIdx;
		}

		if (g_IsCaptionPressActive && (wParam & MK_LBUTTON) && IsInCustomCaptionBand(g_CaptionPressPoint)) {
			int dragX = GetSystemMetrics(SM_CXDRAG);
			int dragY = GetSystemMetrics(SM_CYDRAG);
			if (abs(x - g_CaptionPressPoint.x) >= dragX || abs(y - g_CaptionPressPoint.y) >= dragY) {
				g_IsCaptionDragStarted = true;
				g_IsCaptionPressActive = false;
				g_PressedMenuIndex = -1;
				ReleaseCapture();
				if (g_MenuVisibleState) {
					g_MenuVisibleState = false;
					PlayAnimation(g_VarMenuAlpha, 0.0, 0.2);
				}
				POINT screenPt = pt; ClientToScreen(hwnd, &screenPt);
				SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(screenPt.x, screenPt.y)); return 0;
			}
		}

		uint64_t packed = ((uint64_t)y << 32) | (uint64_t)(uint32_t)x;
		g_LastMousePosPacked.store(packed, std::memory_order_relaxed);
		g_CommandQueue.push(CmdWin32Msg{ hwnd, msg, wParam, lParam });
		g_RenderEvent.Notify();
		return 0;
	}
	case WM_MOUSELEAVE: {
		POINT currMousePt;
		GetCursorPos(&currMousePt);          // 获取鼠标屏幕坐标
		ScreenToClient(hwnd, &currMousePt);  // 转为窗口客户区坐标
		g_bMouseInCanvas.store(false, std::memory_order_relaxed);
		if (!IsInMenuBarBand(currMousePt) && g_MenuVisibleState && !g_IsMenuPopupActive.load()) {
			g_MenuVisibleState = false; 
			g_PressedMenuIndexForRender.store(-1, std::memory_order_release);
			PlayAnimation(g_VarMenuAlpha, 0.0, 0.2);
		}
		if (g_HoveredMenuIndex != -1) { 
			PlayAnimation(g_VarHoverAlpha[g_HoveredMenuIndex], 0.0, 0.15); 
			g_HoveredMenuIndex = -1; 
		}
		g_RenderEvent.Notify(); return 0;
	}
	case WM_LBUTTONDOWN: {
		int y = GET_Y_LPARAM(lParam); int x = GET_X_LPARAM(lParam);
		POINT pt = { x, y };
		g_CommandQueue.push(CmdWin32Msg{ hwnd, msg, wParam, lParam });
		SetFocus(hwnd);
		if (IsInCustomCaptionBand(pt)) {
			SetCapture(hwnd);
			g_CaptionPressPoint = pt;
			g_IsCaptionPressActive = true;
			g_IsCaptionDragStarted = false;
			g_PressedMenuIndex = IsInMenuBarBand(pt) ? GetMenuIndexFromLogicalX((float)x / g_dpiScale) : -1;
			g_RenderEvent.Notify();
			return 0;
		}
		if (x > GetPanelWidth() && !g_ImGuiWantCaptureMouse.load(std::memory_order_relaxed)) {
			if (y > g_CaptionHeight.load()) {
				g_CommandQueue.push(CmdAddHitMark{ (float)x / g_dpiScale, (float)y / g_dpiScale });
			}
		}
		g_RenderEvent.Notify();
		return 0;
	}
	case WM_LBUTTONUP: {
		int y = GET_Y_LPARAM(lParam); int x = GET_X_LPARAM(lParam);
		POINT pt = { x, y };
		g_CommandQueue.push(CmdWin32Msg{ hwnd, msg, wParam, lParam });
		if (GetCapture() == hwnd) {
			ReleaseCapture();
		}
		if (g_IsCaptionPressActive && !g_IsCaptionDragStarted) {
			int releasedMenuIndex = IsInMenuBarBand(pt) ? GetMenuIndexFromLogicalX((float)x / g_dpiScale) : -1;
			if (g_PressedMenuIndex != -1 && g_PressedMenuIndex == releasedMenuIndex) {
				g_IsCaptionPressActive = false;
				g_PressedMenuIndex = -1;
				g_PressedMenuIndexForRender.store(releasedMenuIndex, std::memory_order_release);
				ShowCustomWin32Menu(hwnd, releasedMenuIndex);
				return 0;
			}
		}
		g_IsCaptionPressActive = false;
		g_IsCaptionDragStarted = false;
		g_PressedMenuIndex = -1;
		return 0;
	}
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_MOUSEWHEEL:
	case WM_CHAR:
	case WM_IME_STARTCOMPOSITION:
	case WM_IME_COMPOSITION:
	case WM_IME_ENDCOMPOSITION: {
		g_CommandQueue.push(CmdWin32Msg{ hwnd, msg, wParam, lParam });
		g_RenderEvent.Notify();
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
		const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
		if ((wParam == 'C' || wParam == 'c') && !ctrlDown) { g_CommandQueue.push(CmdResetCanvas{}); g_RenderEvent.Notify(); return 0; }
		g_CommandQueue.push(CmdKeyDown{ wParam, ctrlDown, shiftDown, altDown });
		g_RenderEvent.Notify();
		return 0;
	}
	case WM_SIZE: {
		int width = max(LOWORD(lParam), 1); int height = max(HIWORD(lParam), 1);
		ExtendFrameIntoClient(hwnd);
		if (g_hPanel) {
			int panelW = min(GetPanelWidth(), width);
			SetWindowPos(g_hPanel, HWND_TOP, 0, 0, panelW, height, SWP_SHOWWINDOW);
		}
	}
	case WM_SETCURSOR: {
		if (LOWORD(lParam) == HTCLIENT) {
			POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
			const RECT uiRect{
				g_UiVisualLeft.load(std::memory_order_relaxed),
				g_UiVisualTop.load(std::memory_order_relaxed),
				g_UiVisualRight.load(std::memory_order_relaxed),
				g_UiVisualBottom.load(std::memory_order_relaxed)
			};
			if (PtInRect(&uiRect, pt)) {
				switch (static_cast<UiCursorState>(g_UiCursorState.load(std::memory_order_relaxed))) {
				case UiCursorState::Hand:
					SetCursor(LoadCursor(NULL, IDC_HAND));
					return TRUE;
				case UiCursorState::IBeam:
					SetCursor(LoadCursor(NULL, IDC_IBEAM));
					return TRUE;
				case UiCursorState::Arrow:
				default:
					SetCursor(LoadCursor(NULL, IDC_ARROW));
					return TRUE;
				}
			}
			if (pt.x > GetPanelWidth() && pt.y > g_CaptionHeight.load() && !g_ImGuiWantCaptureMouse.load(std::memory_order_relaxed)) {
				SetCursor(NULL); return TRUE;
			}
			SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE;
		}
		break;
	}
	case WM_DESTROY: {
		g_CommandQueue.push(CmdExit{}); g_isRunning.store(false, std::memory_order_relaxed);
		g_RenderEvent.Notify(); PostQuitMessage(0); return 0;
	}
	case WM_PAINT: {
		static bool s_isFirstPaint = true;
		if (s_isFirstPaint) {
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			RECT rcClient;
			GetClientRect(hwnd, &rcClient);

			// 1. 刷顶部黑区（给 DWM 解锁透明按钮）
			RECT rcCaption = rcClient;
			rcCaption.bottom = g_CaptionHeight.load(std::memory_order_relaxed);
			FillRect(hdc, &rcCaption, (HBRUSH)GetStockObject(BLACK_BRUSH));

			// 标记已完成，以后系统再发 WM_PAINT，直接空过！
			s_isFirstPaint = false;
			EndPaint(hwnd, &ps);
		}
		break;
	}
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 主程序入口
int main() {

#define USE_CONSOLE
#ifdef USE_CONSOLE
	struct Console_Res {
		FILE* stream;

		Console_Res() {
			AllocConsole();
			freopen_s(&stream, "CONOUT$", "w+t", stdout);
			freopen_s(&stream, "CONIN$", "r+t", stdin);
		}

		~Console_Res() {
			FreeConsole();
			std::cout << "cancel console\n";
		}
	} console_res;
#endif
	utils::set_log_color_cout("");
	utils::set_log_color_cerr("");
	utils::set_log_source_location(true);


	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icex);

	// 【首次初始化排版】：先算出物理文字宽度
	CalculateMenuWidths();

	HINSTANCE hInstance = GetModuleHandle(NULL);
	WNDCLASSW wcMain = { 0 }; wcMain.lpfnWndProc = MainWndProc; wcMain.hInstance = hInstance; wcMain.hCursor = LoadCursor(NULL, IDC_ARROW); wcMain.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); wcMain.lpszClassName = L"MainClass"; RegisterClassW(&wcMain);
	WNDCLASSW wcPanel = { 0 }; wcPanel.lpfnWndProc = PanelWndProc; wcPanel.hInstance = hInstance; wcPanel.hbrBackground = NULL; wcPanel.lpszClassName = L"PanelClass"; RegisterClassW(&wcPanel);

	// 【完美适配高分屏】：初始宽度高度调大为 960x600，并自动按屏幕 DPI 放大！
	int initW = 960; int initH = 600;
	HDC screenDC = GetDC(NULL); int sysDpi = GetDeviceCaps(screenDC, LOGPIXELSX); ReleaseDC(NULL, screenDC);
	initW = initW * sysDpi / 96; initH = initH * sysDpi / 96;

	g_hMainWindow = CreateWindowExW(0, wcMain.lpszClassName, L"Modern C++20 Win32 究极大满贯", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, initW, initH, NULL, NULL, hInstance, NULL);
	g_hCanvas = g_hMainWindow;
	int initialPanelWidth = min(GetPanelWidth(), initW);
	g_hPanel = CreateWindowExW(0, wcPanel.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, initialPanelWidth, initH, g_hMainWindow, NULL, hInstance, NULL);

	g_UIThreadId = GetCurrentThreadId();

	std::thread renderThread(RenderThreadFunc);
	g_GpuInitDoneEvent.Wait(0);
	g_RenderThreadReady.store(true, std::memory_order_release); // [新增] 标记渲染线程已就绪

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
