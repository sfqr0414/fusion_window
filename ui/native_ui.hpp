#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <imm.h>
#include <uianimation.h>
#include <windowsx.h>
#include <wrl.h>

namespace fusion::ui {

using Microsoft::WRL::ComPtr;

struct GraphicsContext {
	ComPtr<ID2D1Factory1> d2dFactory;
	ComPtr<ID2D1Device> d2dDevice;
	ComPtr<ID2D1DeviceContext> d2dContext;
	ComPtr<IDWriteFactory> dwriteFactory;
};

struct HostCallbacks {
	std::function<void(bool)> onGridChanged;
	std::function<void(int)> onAimStyleChanged;
	std::function<void(float)> onAimRadiusChanged;
	std::function<void()> onResetCanvas;
};

struct KeyModifiers {
	bool ctrl = false;
	bool shift = false;
	bool alt = false;
};

enum class CursorKind {
	None,
	Arrow,
	Hand,
	IBeam,
};

class UIAnimationSystem;

class UIAnimation {
public:
	UIAnimation() = default;
	explicit UIAnimation(float initialValue) : cachedValue_(initialValue) {}

	void Attach(ComPtr<IUIAnimationVariable> variable) {
		variable_ = std::move(variable);
	}

	void SetObservedValue(float value) {
		cachedValue_ = value;
	}

	float Value() const {
		if (variable_) {
			double value = 0.0;
			if (SUCCEEDED(variable_->GetValue(&value))) {
				return static_cast<float>(value);
			}
		}
		return cachedValue_;
	}

	IUIAnimationVariable* Variable() const {
		return variable_.Get();
	}

private:
	ComPtr<IUIAnimationVariable> variable_;
	float cachedValue_ = 0.0f;
};

class UIAnimationSystem {
public:
	void Initialize() {
		if (initialized_) {
			return;
		}
		initialized_ = true;
		const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		coInitialized_ = SUCCEEDED(initHr);
		if (FAILED(CoCreateInstance(CLSID_UIAnimationManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager_)))) {
			return;
		}
		CoCreateInstance(CLSID_UIAnimationTransitionLibrary, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&library_));
	}

	~UIAnimationSystem() {
		if (coInitialized_) {
			CoUninitialize();
		}
	}

	void Attach(UIAnimation& animation, float initialValue) {
		animation.SetObservedValue(initialValue);
		if (!manager_) {
			return;
		}
		if (animation.Variable()) {
			return;
		}
		ComPtr<IUIAnimationVariable> variable;
		if (SUCCEEDED(manager_->CreateAnimationVariable(initialValue, &variable)) && variable) {
			animation.Attach(std::move(variable));
		}
	}

	void Animate(UIAnimation& animation, float target, double duration = 0.16, double accel = 0.3, double decel = 0.3) {
		animation.SetObservedValue(target);
		if (!manager_ || !library_ || !animation.Variable()) {
			return;
		}
		double current = 0.0;
		if (FAILED(animation.Variable()->GetValue(&current)) || std::fabs(current - static_cast<double>(target)) < 1e-4) {
			return;
		}
		ComPtr<IUIAnimationStoryboard> storyboard;
		ComPtr<IUIAnimationTransition> transition;
		if (FAILED(manager_->CreateStoryboard(&storyboard)) || !storyboard) {
			return;
		}
		if (FAILED(library_->CreateAccelerateDecelerateTransition(duration, target, accel, decel, &transition)) || !transition) {
			return;
		}
		storyboard->AddTransition(animation.Variable(), transition.Get());
		storyboard->Schedule(NowSeconds());
	}

	void Update() {
		if (manager_) {
			manager_->Update(NowSeconds());
		}
	}

private:
	static UI_ANIMATION_SECONDS NowSeconds() {
		using clock = std::chrono::steady_clock;
		return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
	}

	bool initialized_ = false;
	bool coInitialized_ = false;
	ComPtr<IUIAnimationManager> manager_;
	ComPtr<IUIAnimationTransitionLibrary> library_;
};

class UIComponent {
public:
	virtual ~UIComponent() = default;

	void AttachAnimationSystem(UIAnimationSystem* animator) {
		animator_ = animator;
		if (!animator_) {
			return;
		}
		animator_->Attach(hoverAnimation_, hovered_ ? 1.0f : 0.0f);
		animator_->Attach(pressAnimation_, pressed_ ? 1.0f : 0.0f);
		animator_->Attach(focusAnimation_, focused_ ? 1.0f : 0.0f);
		OnAttachAnimations();
	}

	void SetBounds(const D2D1_RECT_F& bounds) {
		bounds_ = bounds;
	}

	const D2D1_RECT_F& Bounds() const {
		return bounds_;
	}

	void SetZIndex(int zIndex) {
		zIndex_ = zIndex;
	}

	int ZIndex() const {
		return zIndex_;
	}

	bool Visible() const {
		return visible_;
	}

	void SetVisible(bool visible) {
		visible_ = visible;
	}

	bool Enabled() const {
		return enabled_;
	}

	void SetEnabled(bool enabled) {
		enabled_ = enabled;
	}

	bool Focused() const {
		return focused_;
	}

	virtual bool IsDynamic() const {
		return false;
	}

	virtual bool IsFocusable() const {
		return false;
	}

	virtual CursorKind Cursor() const {
		return IsFocusable() ? CursorKind::Hand : CursorKind::Arrow;
	}

	virtual void Render(ID2D1DeviceContext* context) = 0;

	virtual void OnHover(bool hovered) {
		hovered_ = hovered;
		Animate(hoverAnimation_, hovered_ ? 1.0f : 0.0f);
	}

	virtual void OnFocus(bool focused) {
		focused_ = focused;
		Animate(focusAnimation_, focused_ ? 1.0f : 0.0f);
	}

	virtual void OnPointerDown(D2D1_POINT_2F) {
		pressed_ = true;
		Animate(pressAnimation_, 1.0f);
	}

	virtual void OnPointerUp(D2D1_POINT_2F) {
		pressed_ = false;
		Animate(pressAnimation_, 0.0f);
	}

	virtual void OnPointerMove(D2D1_POINT_2F) {}
	virtual void OnMouseWheel(int) {}
	virtual void OnKeyDown(WPARAM, const KeyModifiers&) {}
	virtual void OnChar(wchar_t) {}
	virtual void OnImeStart(HWND) {}
	virtual void OnImeComposition(HWND, LPARAM) {}
	virtual void OnImeEnd(HWND) {}

	virtual bool HitTest(ID2D1Factory1* factory, D2D1_POINT_2F point) const {
		if (!visible_ || !enabled_) {
			return false;
		}
		if (!PointInRect(bounds_, point)) {
			return false;
		}
		if (!factory) {
			return true;
		}
		ComPtr<ID2D1Geometry> geometry;
		if (FAILED(CreateHitGeometry(factory, &geometry)) || !geometry) {
			return true;
		}
		BOOL contains = FALSE;
		if (FAILED(geometry->FillContainsPoint(point, nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, &contains))) {
			return true;
		}
		return contains == TRUE;
	}

	static bool PointInRect(const D2D1_RECT_F& rect, D2D1_POINT_2F point) {
		return point.x >= rect.left && point.x <= rect.right && point.y >= rect.top && point.y <= rect.bottom;
	}

protected:
	virtual void OnAttachAnimations() {}

	void Animate(UIAnimation& animation, float target, double duration = 0.16, double accel = 0.3, double decel = 0.3) {
		if (animator_) {
			animator_->Animate(animation, target, duration, accel, decel);
		}
		else {
			animation.SetObservedValue(target);
		}
	}

	float HoverProgress() const {
		return hoverAnimation_.Value();
	}

	float PressProgress() const {
		return pressAnimation_.Value();
	}

	float FocusProgress() const {
		return focusAnimation_.Value();
	}

	virtual HRESULT CreateHitGeometry(ID2D1Factory1* factory, ID2D1Geometry** geometry) const {
		if (!factory || !geometry) {
			return E_INVALIDARG;
		}
		return factory->CreateRectangleGeometry(bounds_, reinterpret_cast<ID2D1RectangleGeometry**>(geometry));
	}

	D2D1_RECT_F bounds_{ D2D1::RectF() };
	int zIndex_ = 0;
	bool visible_ = true;
	bool enabled_ = true;
	bool hovered_ = false;
	bool pressed_ = false;
	bool focused_ = false;
	UIAnimationSystem* animator_ = nullptr;
	UIAnimation hoverAnimation_{};
	UIAnimation pressAnimation_{};
	UIAnimation focusAnimation_{};
};

class LayoutBase {
public:
	virtual ~LayoutBase() = default;
	virtual void Arrange(std::span<UIComponent*> items, const D2D1_RECT_F& bounds, float dpiScale) = 0;
};

class VerticalStackLayout final : public LayoutBase {
public:
	VerticalStackLayout(float padding, float gap) : padding_(padding), gap_(gap) {}

	void Arrange(std::span<UIComponent*> items, const D2D1_RECT_F& bounds, float) override {
		float y = bounds.top + padding_;
		for (auto* item : items) {
			if (!item || !item->Visible()) {
				continue;
			}
			const float height = item->Bounds().bottom - item->Bounds().top;
			item->SetBounds(D2D1::RectF(bounds.left + padding_, y, bounds.right - padding_, y + height));
			y += height + gap_;
		}
	}

private:
	float padding_ = 0.0f;
	float gap_ = 0.0f;
};

namespace detail {

constexpr float kCardRadius = 18.0f;
constexpr float kControlHeight = 36.0f;
constexpr float kCardWidth = 344.0f;
constexpr float kSmallGap = 12.0f;
constexpr float kLineHeight = 18.0f;

inline D2D1_COLOR_F MakeColor(float r, float g, float b, float a = 1.0f) {
	return D2D1::ColorF(r, g, b, a);
}

inline float Clamp01(float value) {
	return (std::clamp)(value, 0.0f, 1.0f);
}

inline D2D1_RECT_F InflateRect(const D2D1_RECT_F& rect, float inset) {
	return D2D1::RectF(rect.left + inset, rect.top + inset, rect.right - inset, rect.bottom - inset);
}

inline float MeasureTextWidth(IDWriteFactory* factory, IDWriteTextFormat* format, std::wstring_view text) {
	if (!factory || !format || text.empty()) {
		return 0.0f;
	}
	ComPtr<IDWriteTextLayout> layout;
	if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 4096.0f, 4096.0f, &layout)) || !layout) {
		return 0.0f;
	}
	DWRITE_TEXT_METRICS metrics{};
	layout->GetMetrics(&metrics);
	return metrics.widthIncludingTrailingWhitespace;
}

inline ComPtr<IDWriteTextLayout> CreateTextLayout(IDWriteFactory* factory, IDWriteTextFormat* format, std::wstring_view text, float width, float height) {
	if (!factory || !format) {
		return nullptr;
	}
	ComPtr<IDWriteTextLayout> layout;
	if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, (std::max)(width, 1.0f), (std::max)(height, 1.0f), &layout))) {
		return nullptr;
	}
	return layout;
}

inline std::wstring GetClipboardText() {
	std::wstring text;
	if (!OpenClipboard(nullptr)) {
		return text;
	}
	HANDLE data = GetClipboardData(CF_UNICODETEXT);
	if (data) {
		const auto* buffer = static_cast<const wchar_t*>(GlobalLock(data));
		if (buffer) {
			text = buffer;
			GlobalUnlock(data);
		}
	}
	CloseClipboard();
	return text;
}

inline bool SetClipboardText(std::wstring_view text) {
	if (!OpenClipboard(nullptr)) {
		return false;
	}
	EmptyClipboard();
	const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (!memory) {
		CloseClipboard();
		return false;
	}
	auto* buffer = static_cast<wchar_t*>(GlobalLock(memory));
	if (!buffer) {
		GlobalFree(memory);
		CloseClipboard();
		return false;
	}
	memcpy(buffer, text.data(), text.size() * sizeof(wchar_t));
	buffer[text.size()] = L'\0';
	GlobalUnlock(memory);
	if (!SetClipboardData(CF_UNICODETEXT, memory)) {
		GlobalFree(memory);
		CloseClipboard();
		return false;
	}
	CloseClipboard();
	return true;
}

class TextBlock final : public UIComponent {
public:
	TextBlock(std::wstring text, ComPtr<IDWriteTextFormat> format, ComPtr<ID2D1SolidColorBrush> brush, bool dynamicText = false)
		: text_(std::move(text)), format_(std::move(format)), brush_(std::move(brush)), dynamicText_(dynamicText) {}

	void SetText(std::wstring text) {
		text_ = std::move(text);
	}

	bool IsDynamic() const override {
		return dynamicText_;
	}

	void Render(ID2D1DeviceContext* context) override {
		if (!visible_ || !format_ || !brush_) {
			return;
		}
		context->DrawTextW(text_.c_str(), static_cast<UINT32>(text_.size()), format_.Get(), bounds_, brush_.Get());
	}

private:
	std::wstring text_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> brush_;
	bool dynamicText_ = false;
};

class CardSurface final : public UIComponent {
public:
	CardSurface(ComPtr<ID2D1SolidColorBrush> fill, ComPtr<ID2D1SolidColorBrush> outline)
		: fill_(std::move(fill)), outline_(std::move(outline)) {}

	void Render(ID2D1DeviceContext* context) override {
		if (!visible_ || !fill_ || !outline_) {
			return;
		}
		auto rounded = D2D1::RoundedRect(bounds_, kCardRadius, kCardRadius);
		context->FillRoundedRectangle(rounded, fill_.Get());
		context->DrawRoundedRectangle(rounded, outline_.Get(), 1.0f);
	}

protected:
	HRESULT CreateHitGeometry(ID2D1Factory1* factory, ID2D1Geometry** geometry) const override {
		if (!factory || !geometry) {
			return E_INVALIDARG;
		}
		return factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(bounds_, kCardRadius, kCardRadius), reinterpret_cast<ID2D1RoundedRectangleGeometry**>(geometry));
	}

private:
	ComPtr<ID2D1SolidColorBrush> fill_;
	ComPtr<ID2D1SolidColorBrush> outline_;
};

class Button final : public UIComponent {
public:
	Button(std::wstring text,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> primaryFill,
		ComPtr<ID2D1SolidColorBrush> secondaryFill,
		ComPtr<ID2D1SolidColorBrush> outline,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		std::function<void()> onClick,
		bool primary)
		: text_(std::move(text)), format_(std::move(format)), primaryFill_(std::move(primaryFill)), secondaryFill_(std::move(secondaryFill)), outline_(std::move(outline)), textBrush_(std::move(textBrush)), onClick_(std::move(onClick)), primary_(primary) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }
	CursorKind Cursor() const override { return CursorKind::IBeam; }

	void Render(ID2D1DeviceContext* context) override {
		auto rounded = D2D1::RoundedRect(bounds_, 12.0f, 12.0f);
		auto* fill = primary_ ? primaryFill_.Get() : secondaryFill_.Get();
		const float hover = HoverProgress();
		const float press = PressProgress();
		fill->SetOpacity(1.0f - 0.18f * hover - 0.12f * press);
		context->FillRoundedRectangle(rounded, fill);
		outline_->SetOpacity(0.55f + 0.45f * FocusProgress());
		context->DrawRoundedRectangle(rounded, outline_.Get(), 1.0f + FocusProgress());
		textBrush_->SetOpacity(enabled_ ? 1.0f : 0.5f);
		context->DrawTextW(text_.c_str(), static_cast<UINT32>(text_.size()), format_.Get(), bounds_, textBrush_.Get());
	}

	void OnPointerDown(D2D1_POINT_2F point) override {
		UIComponent::OnPointerDown(point);
	}

	void OnPointerUp(D2D1_POINT_2F point) override {
		bool shouldFire = pressed_ && PointInRect(bounds_, point);
		UIComponent::OnPointerUp(point);
		if (shouldFire && onClick_) {
			onClick_();
		}
	}

	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if ((key == VK_SPACE || key == VK_RETURN) && onClick_) {
			onClick_();
		}
	}

private:
	std::wstring text_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> primaryFill_;
	ComPtr<ID2D1SolidColorBrush> secondaryFill_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	std::function<void()> onClick_;
	bool primary_ = false;
};

class ImageFrame final : public UIComponent {
public:
	ImageFrame(ComPtr<ID2D1SolidColorBrush> outline, ComPtr<ID2D1SolidColorBrush> accent, ComPtr<ID2D1SolidColorBrush> muted)
		: outline_(std::move(outline)), accent_(std::move(accent)), muted_(std::move(muted)) {}

	void Render(ID2D1DeviceContext* context) override {
		auto rounded = D2D1::RoundedRect(bounds_, 14.0f, 14.0f);
		context->FillRoundedRectangle(rounded, muted_.Get());
		for (int row = 0; row < 4; ++row) {
			for (int col = 0; col < 6; ++col) {
				const float tileW = (bounds_.right - bounds_.left - 24.0f) / 6.0f;
				const float tileH = (bounds_.bottom - bounds_.top - 24.0f) / 4.0f;
				const float left = bounds_.left + 12.0f + tileW * col;
				const float top = bounds_.top + 12.0f + tileH * row;
				const float alpha = ((row + col) % 2 == 0) ? 0.22f : 0.10f;
				accent_->SetOpacity(alpha);
				context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, left + tileW - 6.0f, top + tileH - 6.0f), 8.0f, 8.0f), accent_.Get());
			}
		}
		outline_->SetOpacity(0.55f);
		context->DrawRoundedRectangle(rounded, outline_.Get(), 1.0f);
	}

private:
	ComPtr<ID2D1SolidColorBrush> outline_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> muted_;
};

class ImageButton final : public UIComponent {
public:
	ImageButton(std::wstring text,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> outline,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		std::function<void()> onClick)
		: text_(std::move(text)), format_(std::move(format)), surface_(std::move(surface)), accent_(std::move(accent)), outline_(std::move(outline)), textBrush_(std::move(textBrush)), onClick_(std::move(onClick)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void Render(ID2D1DeviceContext* context) override {
		auto rounded = D2D1::RoundedRect(bounds_, 12.0f, 12.0f);
		surface_->SetOpacity(1.0f - 0.12f * HoverProgress() - 0.08f * PressProgress());
		context->FillRoundedRectangle(rounded, surface_.Get());
		outline_->SetOpacity(0.55f + 0.45f * FocusProgress());
		context->DrawRoundedRectangle(rounded, outline_.Get(), 1.0f + FocusProgress());
		const D2D1_RECT_F imageRect = D2D1::RectF(bounds_.left + 12.0f, bounds_.top + 8.0f, bounds_.left + 48.0f, bounds_.bottom - 8.0f);
		accent_->SetOpacity(0.25f);
		context->FillRoundedRectangle(D2D1::RoundedRect(imageRect, 8.0f, 8.0f), accent_.Get());
		accent_->SetOpacity(0.85f);
		context->DrawLine(D2D1::Point2F(imageRect.left + 8.0f, imageRect.bottom - 10.0f), D2D1::Point2F(imageRect.left + 18.0f, imageRect.top + 16.0f), accent_.Get(), 2.0f);
		context->DrawLine(D2D1::Point2F(imageRect.left + 18.0f, imageRect.top + 16.0f), D2D1::Point2F(imageRect.right - 8.0f, imageRect.bottom - 12.0f), accent_.Get(), 2.0f);
		context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(imageRect.right - 12.0f, imageRect.top + 12.0f), 4.0f, 4.0f), accent_.Get());
		context->DrawTextW(text_.c_str(), static_cast<UINT32>(text_.size()), format_.Get(), D2D1::RectF(imageRect.right + 12.0f, bounds_.top, bounds_.right - 12.0f, bounds_.bottom), textBrush_.Get());
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); }
	void OnPointerUp(D2D1_POINT_2F point) override {
		bool shouldFire = pressed_ && PointInRect(bounds_, point);
		UIComponent::OnPointerUp(point);
		if (shouldFire && onClick_) {
			onClick_();
		}
	}
	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if ((key == VK_SPACE || key == VK_RETURN) && onClick_) {
			onClick_();
		}
	}

private:
	std::wstring text_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	std::function<void()> onClick_;
};

class Checkbox final : public UIComponent {
public:
	Checkbox(std::wstring text,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		ComPtr<ID2D1SolidColorBrush> outline,
		bool initialValue,
		std::function<void(bool)> onChanged)
		: text_(std::move(text)), format_(std::move(format)), surface_(std::move(surface)), accent_(std::move(accent)), textBrush_(std::move(textBrush)), outline_(std::move(outline)), checked_(initialValue), onChanged_(std::move(onChanged)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(checkAnimation_, checked_ ? 1.0f : 0.0f);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		D2D1_RECT_F box = D2D1::RectF(bounds_.left, bounds_.top + 7.0f, bounds_.left + 22.0f, bounds_.top + 29.0f);
		context->FillRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), surface_.Get());
		outline_->SetOpacity(0.55f + 0.45f * FocusProgress());
		context->DrawRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), outline_.Get(), 1.0f + FocusProgress());
		const float check = checkAnimation_.Value();
		if (check > 0.01f) {
			accent_->SetOpacity(0.45f + 0.35f * check + 0.2f * HoverProgress());
			context->FillRoundedRectangle(D2D1::RoundedRect(InflateRect(box, 3.0f), 4.0f, 4.0f), accent_.Get());
		}
		context->DrawTextW(text_.c_str(), static_cast<UINT32>(text_.size()), format_.Get(), D2D1::RectF(box.right + 12.0f, bounds_.top, bounds_.right, bounds_.bottom), textBrush_.Get());
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); }
	void OnPointerUp(D2D1_POINT_2F point) override {
		if (pressed_ && PointInRect(bounds_, point)) {
			checked_ = !checked_;
			Animate(checkAnimation_, checked_ ? 1.0f : 0.0f, 0.14);
			if (onChanged_) {
				onChanged_(checked_);
			}
		}
		UIComponent::OnPointerUp(point);
	}
	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_SPACE) {
			checked_ = !checked_;
			Animate(checkAnimation_, checked_ ? 1.0f : 0.0f, 0.14);
			if (onChanged_) {
				onChanged_(checked_);
			}
		}
	}

private:
	std::wstring text_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	bool checked_ = false;
	UIAnimation checkAnimation_{};
	std::function<void(bool)> onChanged_;
};

class RadioButton final : public UIComponent {
public:
	RadioButton(std::wstring text,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		ComPtr<ID2D1SolidColorBrush> outline,
		std::shared_ptr<int> selectedValue,
		int ownValue,
		std::function<void(int)> onChanged)
		: text_(std::move(text)), format_(std::move(format)), surface_(std::move(surface)), accent_(std::move(accent)), textBrush_(std::move(textBrush)), outline_(std::move(outline)), selectedValue_(std::move(selectedValue)), ownValue_(ownValue), onChanged_(std::move(onChanged)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(selectionAnimation_, *selectedValue_ == ownValue_ ? 1.0f : 0.0f);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		Animate(selectionAnimation_, *selectedValue_ == ownValue_ ? 1.0f : 0.0f, 0.10);
		const float cx = bounds_.left + 11.0f;
		const float cy = bounds_.top + 18.0f;
		context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 10.0f, 10.0f), surface_.Get());
		outline_->SetOpacity(0.55f + 0.45f * FocusProgress());
		context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 10.0f, 10.0f), outline_.Get(), 1.0f + FocusProgress());
		const float selected = selectionAnimation_.Value();
		if (selected > 0.01f) {
			accent_->SetOpacity(0.5f + 0.3f * selected + 0.2f * HoverProgress());
			context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f + selected * 2.0f, 3.0f + selected * 2.0f), accent_.Get());
		}
		context->DrawTextW(text_.c_str(), static_cast<UINT32>(text_.size()), format_.Get(), D2D1::RectF(bounds_.left + 30.0f, bounds_.top, bounds_.right, bounds_.bottom), textBrush_.Get());
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); }
	void OnPointerUp(D2D1_POINT_2F point) override {
		if (pressed_ && PointInRect(bounds_, point)) {
			*selectedValue_ = ownValue_;
			Animate(selectionAnimation_, 1.0f, 0.14);
			if (onChanged_) {
				onChanged_(ownValue_);
			}
		}
		UIComponent::OnPointerUp(point);
	}
	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_SPACE || key == VK_RETURN) {
			*selectedValue_ = ownValue_;
			Animate(selectionAnimation_, 1.0f, 0.14);
			if (onChanged_) {
				onChanged_(ownValue_);
			}
		}
	}

protected:
	HRESULT CreateHitGeometry(ID2D1Factory1* factory, ID2D1Geometry** geometry) const override {
		if (!factory || !geometry) {
			return E_INVALIDARG;
		}
		return factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(bounds_.left + 11.0f, bounds_.top + 18.0f), 12.0f, 12.0f), reinterpret_cast<ID2D1EllipseGeometry**>(geometry));
	}

private:
	std::wstring text_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	std::shared_ptr<int> selectedValue_;
	int ownValue_ = 0;
	UIAnimation selectionAnimation_{};
	std::function<void(int)> onChanged_;
};

class Slider final : public UIComponent {
public:
	Slider(std::wstring label,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		ComPtr<ID2D1SolidColorBrush> outline,
		float initialValue,
		std::function<void(float)> onChanged)
		: label_(std::move(label)), format_(std::move(format)), surface_(std::move(surface)), accent_(std::move(accent)), textBrush_(std::move(textBrush)), outline_(std::move(outline)), value_(Clamp01(initialValue)), onChanged_(std::move(onChanged)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(valueAnimation_, value_);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		context->DrawTextW(label_.c_str(), static_cast<UINT32>(label_.size()), format_.Get(), D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f), textBrush_.Get());
		D2D1_RECT_F track = TrackBounds();
		context->FillRoundedRectangle(D2D1::RoundedRect(track, 8.0f, 8.0f), surface_.Get());
		const float value = valueAnimation_.Value();
		D2D1_RECT_F fill = D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * value, track.bottom);
		context->FillRoundedRectangle(D2D1::RoundedRect(fill, 8.0f, 8.0f), accent_.Get());
		outline_->SetOpacity(0.45f + 0.55f * FocusProgress());
		context->DrawRoundedRectangle(D2D1::RoundedRect(track, 8.0f, 8.0f), outline_.Get(), 1.0f + FocusProgress());
		float handleX = fill.right;
		const float handleRadius = 7.0f + HoverProgress() + PressProgress();
		context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(handleX, (track.top + track.bottom) * 0.5f), handleRadius, handleRadius), outline_.Get());
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); UpdateValue(point); }
	void OnPointerMove(D2D1_POINT_2F point) override { if (pressed_) UpdateValue(point); }
	void OnPointerUp(D2D1_POINT_2F point) override { if (pressed_) UpdateValue(point); UIComponent::OnPointerUp(point); }
	void OnMouseWheel(int delta) override { SetValue(value_ + (delta > 0 ? 0.03f : -0.03f)); }
	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_LEFT || key == VK_DOWN) SetValue(value_ - 0.02f);
		if (key == VK_RIGHT || key == VK_UP) SetValue(value_ + 0.02f);
	}

private:
	D2D1_RECT_F TrackBounds() const {
		return D2D1::RectF(bounds_.left, bounds_.top + 20.0f, bounds_.right, bounds_.top + 32.0f);
	}
	void UpdateValue(D2D1_POINT_2F point) {
		D2D1_RECT_F track = TrackBounds();
		float width = (std::max)(1.0f, track.right - track.left);
		SetValue((point.x - track.left) / width);
	}
	void SetValue(float value) {
		value_ = Clamp01(value);
		Animate(valueAnimation_, value_, 0.12);
		if (onChanged_) onChanged_(value_);
	}

	std::wstring label_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	float value_ = 0.0f;
	UIAnimation valueAnimation_{};
	std::function<void(float)> onChanged_;
};

class ProgressBar final : public UIComponent {
public:
	ProgressBar(std::wstring label,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		UIAnimation* animation)
		: label_(std::move(label)), format_(std::move(format)), surface_(std::move(surface)), accent_(std::move(accent)), textBrush_(std::move(textBrush)), animation_(animation) {}

	bool IsDynamic() const override { return true; }

	void Render(ID2D1DeviceContext* context) override {
		std::wstringstream ss;
		float progress = Clamp01(animation_ ? animation_->Value() : 0.0f);
		ss << label_ << L"  " << static_cast<int>(progress * 100.0f) << L"%";
		auto text = ss.str();
		context->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format_.Get(), D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f), textBrush_.Get());
		D2D1_RECT_F track = D2D1::RectF(bounds_.left, bounds_.top + 20.0f, bounds_.right, bounds_.top + 32.0f);
		context->FillRoundedRectangle(D2D1::RoundedRect(track, 8.0f, 8.0f), surface_.Get());
		D2D1_RECT_F fill = D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * progress, track.bottom);
		context->FillRoundedRectangle(D2D1::RoundedRect(fill, 8.0f, 8.0f), accent_.Get());
	}

private:
	std::wstring label_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	UIAnimation* animation_ = nullptr;
};

class TextInput final : public UIComponent {
public:
	TextInput(std::wstring label,
		std::wstring placeholder,
		std::wstring initialText,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> outline,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		ComPtr<ID2D1SolidColorBrush> mutedBrush,
		ComPtr<ID2D1SolidColorBrush> selectionBrush,
		bool multiline,
		std::function<void(std::wstring_view)> onChanged)
		: label_(std::move(label)), placeholder_(std::move(placeholder)), text_(std::move(initialText)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surface_(std::move(surface)), outline_(std::move(outline)), textBrush_(std::move(textBrush)), mutedBrush_(std::move(mutedBrush)), selectionBrush_(std::move(selectionBrush)), multiline_(multiline), onChanged_(std::move(onChanged)) {
		caret_ = text_.size();
			selectionAnchor_ = caret_;
	}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void Render(ID2D1DeviceContext* context) override {
		ComPtr<ID2D1SolidColorBrush> localSurface;
		ComPtr<ID2D1SolidColorBrush> localOutline;
		ComPtr<ID2D1SolidColorBrush> localText;
		ComPtr<ID2D1SolidColorBrush> localMuted;
		ComPtr<ID2D1SolidColorBrush> localSelection;
		context->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.985f, 0.995f, 0.98f), &localSurface);
		context->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.42f, 0.78f, 0.85f), &localOutline);
		context->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.12f, 0.18f, 1.0f), &localText);
		context->CreateSolidColorBrush(D2D1::ColorF(0.32f, 0.36f, 0.44f, 1.0f), &localMuted);
		context->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.42f, 0.78f, 0.22f), &localSelection);
		auto* textBrush = localText ? localText.Get() : textBrush_.Get();
		auto* mutedBrush = localMuted ? localMuted.Get() : mutedBrush_.Get();
		auto* surfaceBrush = localSurface ? localSurface.Get() : surface_.Get();
		auto* outlineBrush = localOutline ? localOutline.Get() : outline_.Get();
		auto* selectionBrush = localSelection ? localSelection.Get() : selectionBrush_.Get();
		context->DrawTextW(label_.c_str(), static_cast<UINT32>(label_.size()), format_.Get(), D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f), mutedBrush);
		D2D1_RECT_F box = TextBounds();
		context->FillRoundedRectangle(D2D1::RoundedRect(box, 12.0f, 12.0f), surfaceBrush);
		outlineBrush->SetOpacity(0.45f + FocusProgress() * 0.55f);
		context->DrawRoundedRectangle(D2D1::RoundedRect(box, 12.0f, 12.0f), outlineBrush, 1.0f + FocusProgress());
		D2D1_RECT_F content = InflateRect(box, 12.0f);
		context->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		const auto renderText = DisplayText();
		const auto layout = CreateTextLayout(dwriteFactory_.Get(), format_.Get(), renderText, content.right - content.left, content.bottom - content.top);
		DrawSelection(context, layout.Get(), content, selectionBrush);
		if (renderText.empty()) {
			context->DrawTextW(placeholder_.c_str(), static_cast<UINT32>(placeholder_.size()), format_.Get(), content, mutedBrush);
		}
		else {
			context->DrawTextLayout(D2D1::Point2F(content.left, content.top), layout.Get(), textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
			DrawImeUnderline(context, layout.Get(), content, outlineBrush);
		}
		if (focused_) {
			DrawCaret(context, layout.Get(), content, outlineBrush);
		}
		context->PopAxisAlignedClip();
	}

	void OnPointerDown(D2D1_POINT_2F point) override {
		UIComponent::OnPointerDown(point);
		draggingSelection_ = true;
		const size_t index = HitTestText(point);
		caret_ = index;
		selectionAnchor_ = index;
	}

	void OnPointerMove(D2D1_POINT_2F point) override {
		if (draggingSelection_) {
			caret_ = HitTestText(point);
		}
	}

	void OnPointerUp(D2D1_POINT_2F point) override {
		UIComponent::OnPointerUp(point);
		draggingSelection_ = false;
	}

	void OnKeyDown(WPARAM key, const KeyModifiers& modifiers) override {
		if (modifiers.ctrl) {
			switch (key) {
			case 'A':
			case 'a':
				selectionAnchor_ = 0;
				caret_ = text_.size();
				return;
			case 'C':
			case 'c':
				if (HasSelection()) {
					SetClipboardText(std::wstring_view(text_.data() + SelectionStart(), SelectionLength()));
				}
				return;
			case 'X':
			case 'x':
				if (HasSelection()) {
					SetClipboardText(std::wstring_view(text_.data() + SelectionStart(), SelectionLength()));
					DeleteSelection();
				}
				return;
			case 'V':
			case 'v':
				InsertText(GetClipboardText());
				return;
			default:
				break;
			}
		}
		switch (key) {
		case VK_BACK:
			if (DeleteSelection()) {
				return;
			}
			if (caret_ > 0) {
				text_.erase(caret_ - 1, 1);
				--caret_;
				selectionAnchor_ = caret_;
				NotifyChanged();
			}
			break;
		case VK_DELETE:
			if (DeleteSelection()) {
				return;
			}
			if (caret_ < text_.size()) {
				text_.erase(caret_, 1);
				NotifyChanged();
			}
			break;
		case VK_LEFT:
			MoveCaret(caret_ > 0 ? caret_ - 1 : 0, modifiers.shift);
			break;
		case VK_RIGHT:
			MoveCaret((std::min)(caret_ + 1, text_.size()), modifiers.shift);
			break;
		case VK_HOME:
			MoveCaret(0, modifiers.shift);
			break;
		case VK_END:
			MoveCaret(text_.size(), modifiers.shift);
			break;
		case VK_UP:
			if (multiline_) {
				MoveCaret(MoveVertical(-1), modifiers.shift);
			}
			break;
		case VK_DOWN:
			if (multiline_) {
				MoveCaret(MoveVertical(1), modifiers.shift);
			}
			break;
		default:
			break;
		}
	}

	void OnChar(wchar_t ch) override {
		if (ch == L'\r') {
			if (multiline_) {
				InsertText(L"\n");
			}
			return;
		}
		if (ch == L'\b' || ch == 0x7F) {
			return;
		}
		if (!multiline_ && (ch == L'\n' || ch == L'\t')) {
			return;
		}
		if (std::iswprint(ch) || (multiline_ && ch == L'\n')) {
			std::wstring value(1, ch);
			InsertText(value);
		}
	}

	void OnFocus(bool focused) override {
		UIComponent::OnFocus(focused);
		if (!focused) {
			draggingSelection_ = false;
		}
	}

	void OnImeStart(HWND) override {
		imeActive_ = true;
		compositionAnchor_ = SelectionStart();
		compositionReplaceLength_ = HasSelection() ? SelectionLength() : 0;
		imeComposition_.clear();
		imeCursorPos_ = 0;
	}

	void OnImeComposition(HWND hwnd, LPARAM lParam) override {
		if (!imeActive_) {
			OnImeStart(hwnd);
		}
		HIMC context = ImmGetContext(hwnd);
		if (!context) {
			return;
		}
		std::wstring result;
		std::wstring composition;
		LONG cursorPos = 0;
		if ((lParam & GCS_RESULTSTR) != 0) {
			const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
			if (bytes > 0) {
				result.resize(static_cast<size_t>(bytes / sizeof(wchar_t)));
				ImmGetCompositionStringW(context, GCS_RESULTSTR, result.data(), bytes);
			}
		}
		if ((lParam & GCS_COMPSTR) != 0) {
			const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
			if (bytes > 0) {
				composition.resize(static_cast<size_t>(bytes / sizeof(wchar_t)));
				ImmGetCompositionStringW(context, GCS_COMPSTR, composition.data(), bytes);
			}
		}
		if ((lParam & GCS_CURSORPOS) != 0) {
			ImmGetCompositionStringW(context, GCS_CURSORPOS, &cursorPos, sizeof(cursorPos));
		}
		ImmReleaseContext(hwnd, context);
		if (!result.empty()) {
			ReplaceRange(compositionAnchor_, compositionReplaceLength_, result);
			imeComposition_.clear();
			imeActive_ = false;
			compositionReplaceLength_ = 0;
			return;
		}
		imeComposition_ = std::move(composition);
		imeCursorPos_ = cursorPos;
	}

	void OnImeEnd(HWND) override {
		imeActive_ = false;
		imeComposition_.clear();
		compositionReplaceLength_ = 0;
	}

	const std::wstring& Text() const { return text_; }

private:
	bool HasSelection() const {
		return caret_ != selectionAnchor_;
	}

	size_t SelectionStart() const {
		return (std::min)(caret_, selectionAnchor_);
	}

	size_t SelectionLength() const {
		return (std::max)(caret_, selectionAnchor_) - SelectionStart();
	}

	D2D1_RECT_F TextBounds() const {
		return D2D1::RectF(bounds_.left, bounds_.top + 20.0f, bounds_.right, bounds_.bottom);
	}

	std::wstring DisplayText() const {
		if (!imeActive_ || imeComposition_.empty()) {
			return text_;
		}
		std::wstring display = text_;
		display.replace(compositionAnchor_, compositionReplaceLength_, imeComposition_);
		return display;
	}

	size_t DisplayCaret() const {
		if (!imeActive_) {
			return caret_;
		}
		return compositionAnchor_ + (std::min)(static_cast<size_t>((std::max)(0L, imeCursorPos_)), imeComposition_.size());
	}

	void DrawSelection(ID2D1DeviceContext* context, IDWriteTextLayout* layout, const D2D1_RECT_F& content, ID2D1SolidColorBrush* selectionBrush) {
		if (!layout || imeActive_ || !HasSelection()) {
			return;
		}
		std::array<DWRITE_HIT_TEST_METRICS, 8> stackMetrics{};
		UINT32 actualCount = 0;
		const UINT32 length = static_cast<UINT32>(SelectionLength());
		HRESULT hr = layout->HitTestTextRange(static_cast<UINT32>(SelectionStart()), length, content.left, content.top, stackMetrics.data(), static_cast<UINT32>(stackMetrics.size()), &actualCount);
		std::vector<DWRITE_HIT_TEST_METRICS> dynamicMetrics;
		if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
			dynamicMetrics.resize(actualCount);
			if (SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(SelectionStart()), length, content.left, content.top, dynamicMetrics.data(), actualCount, &actualCount))) {
				for (const auto& metric : dynamicMetrics) {
					selectionBrush->SetOpacity(0.18f + FocusProgress() * 0.18f);
					context->FillRectangle(D2D1::RectF(metric.left, metric.top, metric.left + metric.width, metric.top + metric.height), selectionBrush);
				}
			}
			return;
		}
		if (SUCCEEDED(hr)) {
			for (UINT32 index = 0; index < actualCount; ++index) {
				const auto& metric = stackMetrics[index];
				selectionBrush->SetOpacity(0.18f + FocusProgress() * 0.18f);
				context->FillRectangle(D2D1::RectF(metric.left, metric.top, metric.left + metric.width, metric.top + metric.height), selectionBrush);
			}
		}
	}

	void DrawImeUnderline(ID2D1DeviceContext* context, IDWriteTextLayout* layout, const D2D1_RECT_F& content, ID2D1SolidColorBrush* outlineBrush) {
		if (!layout || !imeActive_ || imeComposition_.empty()) {
			return;
		}
		std::array<DWRITE_HIT_TEST_METRICS, 8> stackMetrics{};
		UINT32 actualCount = 0;
		const UINT32 start = static_cast<UINT32>(compositionAnchor_);
		const UINT32 length = static_cast<UINT32>(imeComposition_.size());
		HRESULT hr = layout->HitTestTextRange(start, length, content.left, content.top, stackMetrics.data(), static_cast<UINT32>(stackMetrics.size()), &actualCount);
		if (FAILED(hr)) {
			return;
		}
		for (UINT32 index = 0; index < actualCount; ++index) {
			const auto& metric = stackMetrics[index];
			context->DrawLine(D2D1::Point2F(metric.left, metric.top + metric.height + 1.0f), D2D1::Point2F(metric.left + metric.width, metric.top + metric.height + 1.0f), outlineBrush, 1.0f);
		}
	}

	void DrawCaret(ID2D1DeviceContext* context, IDWriteTextLayout* layout, const D2D1_RECT_F& content, ID2D1SolidColorBrush* outlineBrush) {
		if (!layout) {
			return;
		}
		FLOAT x = content.left;
		FLOAT y = content.top;
		DWRITE_HIT_TEST_METRICS metrics{};
		if (FAILED(layout->HitTestTextPosition(static_cast<UINT32>(DisplayCaret()), FALSE, &x, &y, &metrics))) {
			return;
		}
		context->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x, y + (std::max)(metrics.height, 16.0f)), outlineBrush, 1.5f);
	}

	size_t HitTestText(D2D1_POINT_2F point) const {
		const D2D1_RECT_F content = InflateRect(TextBounds(), 12.0f);
		const auto layout = CreateTextLayout(dwriteFactory_.Get(), format_.Get(), text_, content.right - content.left, content.bottom - content.top);
		if (!layout) {
			return text_.size();
		}
		BOOL trailing = FALSE;
		BOOL inside = FALSE;
		DWRITE_HIT_TEST_METRICS metrics{};
		const float localX = point.x - content.left;
		const float localY = point.y - content.top;
		if (FAILED(layout->HitTestPoint(localX, localY, &trailing, &inside, &metrics))) {
			return text_.size();
		}
		size_t index = metrics.textPosition + (trailing ? metrics.length : 0);
		return (std::min)(index, text_.size());
	}

	bool DeleteSelection() {
		if (!HasSelection()) {
			return false;
		}
		const size_t start = SelectionStart();
		text_.erase(start, SelectionLength());
		caret_ = start;
		selectionAnchor_ = start;
		NotifyChanged();
		return true;
	}

	void ReplaceRange(size_t start, size_t length, std::wstring_view replacement) {
		text_.replace(start, length, replacement.data(), replacement.size());
		caret_ = start + replacement.size();
		selectionAnchor_ = caret_;
		NotifyChanged();
	}

	void InsertText(std::wstring_view value) {
		if (value.empty()) {
			return;
		}
		const size_t start = HasSelection() ? SelectionStart() : caret_;
		const size_t length = HasSelection() ? SelectionLength() : 0;
		ReplaceRange(start, length, value);
		imeActive_ = false;
		imeComposition_.clear();
		compositionReplaceLength_ = 0;
	}

	void MoveCaret(size_t index, bool extendSelection) {
		caret_ = (std::min)(index, text_.size());
		if (!extendSelection) {
			selectionAnchor_ = caret_;
		}
	}

	size_t MoveVertical(int direction) const {
		const size_t currentLineStart = text_.rfind(L'\n', caret_ == 0 ? 0 : caret_ - 1);
		const size_t lineStart = currentLineStart == std::wstring::npos ? 0 : currentLineStart + 1;
		const size_t lineEnd = text_.find(L'\n', caret_);
		const size_t column = caret_ - lineStart;
		if (direction < 0) {
			if (lineStart == 0) {
				return 0;
			}
			const size_t prevEnd = lineStart - 1;
			const size_t prevBreak = text_.rfind(L'\n', prevEnd == 0 ? 0 : prevEnd - 1);
			const size_t prevStart = prevBreak == std::wstring::npos ? 0 : prevBreak + 1;
			return prevStart + (std::min)(column, prevEnd - prevStart);
		}
		if (lineEnd == std::wstring::npos) {
			return text_.size();
		}
		const size_t nextStart = lineEnd + 1;
		const size_t nextEnd = text_.find(L'\n', nextStart);
		const size_t boundedNextEnd = nextEnd == std::wstring::npos ? text_.size() : nextEnd;
		return nextStart + (std::min)(column, boundedNextEnd - nextStart);
	}

	void NotifyChanged() {
		if (onChanged_) {
			onChanged_(text_);
		}
	}

	std::wstring label_;
	std::wstring placeholder_;
	std::wstring text_;
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	ComPtr<ID2D1SolidColorBrush> mutedBrush_;
	ComPtr<ID2D1SolidColorBrush> selectionBrush_;
	bool multiline_ = false;
	size_t caret_ = 0;
	size_t selectionAnchor_ = 0;
	bool draggingSelection_ = false;
	bool imeActive_ = false;
	std::wstring imeComposition_;
	LONG imeCursorPos_ = 0;
	size_t compositionAnchor_ = 0;
	size_t compositionReplaceLength_ = 0;
	std::function<void(std::wstring_view)> onChanged_;
};

class ListBox final : public UIComponent {
public:
	ListBox(std::vector<std::wstring> items,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> outline,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		std::function<void(std::wstring_view)> onChanged)
		: items_(std::move(items)), format_(std::move(format)), surface_(std::move(surface)), accent_(std::move(accent)), outline_(std::move(outline)), textBrush_(std::move(textBrush)), onChanged_(std::move(onChanged)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void SetScrollNormalized(float value) {
		const size_t maxOffset = items_.size() > VisibleRows() ? items_.size() - VisibleRows() : 0;
		topIndex_ = static_cast<size_t>(Clamp01(value) * static_cast<float>(maxOffset));
	}

	float ScrollNormalized() const {
		const size_t maxOffset = items_.size() > VisibleRows() ? items_.size() - VisibleRows() : 0;
		if (maxOffset == 0) {
			return 0.0f;
		}
		return static_cast<float>(topIndex_) / static_cast<float>(maxOffset);
	}

	void Render(ID2D1DeviceContext* context) override {
		context->FillRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), surface_.Get());
		outline_->SetOpacity(0.5f + 0.5f * FocusProgress());
		context->DrawRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), outline_.Get(), 1.0f + FocusProgress());
		D2D1_RECT_F content = InflateRect(bounds_, 10.0f);
		context->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		for (size_t row = 0; row < VisibleRows(); ++row) {
			size_t index = topIndex_ + row;
			if (index >= items_.size()) {
				break;
			}
			D2D1_RECT_F rowRect = D2D1::RectF(content.left, content.top + row * 22.0f, content.right, content.top + row * 22.0f + 20.0f);
			if (index == selectedIndex_) {
				accent_->SetOpacity(0.16f + 0.12f * FocusProgress());
				context->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), accent_.Get());
			}
			if (hoveredRow_ && *hoveredRow_ == index) {
				accent_->SetOpacity(0.08f + 0.08f * HoverProgress());
				context->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), accent_.Get());
			}
			context->DrawTextW(items_[index].c_str(), static_cast<UINT32>(items_[index].size()), format_.Get(), rowRect, textBrush_.Get());
		}
		context->PopAxisAlignedClip();
	}

	void OnPointerMove(D2D1_POINT_2F point) override {
		hoveredRow_ = RowFromPoint(point);
	}

	void OnPointerDown(D2D1_POINT_2F point) override {
		UIComponent::OnPointerDown(point);
		SelectRow(RowFromPoint(point));
	}

	void OnPointerUp(D2D1_POINT_2F point) override { UIComponent::OnPointerUp(point); }

	void OnMouseWheel(int delta) override {
		if (delta > 0 && topIndex_ > 0) {
			--topIndex_;
		}
		else if (delta < 0 && topIndex_ + VisibleRows() < items_.size()) {
			++topIndex_;
		}
	}

	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (items_.empty()) {
			return;
		}
		if (key == VK_UP && selectedIndex_ > 0) {
			SelectIndex(selectedIndex_ - 1);
		}
		if (key == VK_DOWN && selectedIndex_ + 1 < items_.size()) {
			SelectIndex(selectedIndex_ + 1);
		}
	}

private:
	size_t VisibleRows() const {
		return static_cast<size_t>((std::max)(1.0f, std::floor((bounds_.bottom - bounds_.top - 20.0f) / 22.0f)));
	}

	std::optional<size_t> RowFromPoint(D2D1_POINT_2F point) const {
		if (!PointInRect(bounds_, point)) {
			return std::nullopt;
		}
		const float relativeY = point.y - bounds_.top - 10.0f;
		if (relativeY < 0.0f) {
			return std::nullopt;
		}
		size_t row = static_cast<size_t>(relativeY / 22.0f);
		if (row >= VisibleRows()) {
			return std::nullopt;
		}
		return topIndex_ + row;
	}

	void SelectRow(std::optional<size_t> row) {
		if (row && *row < items_.size()) {
			SelectIndex(*row);
		}
	}

	void SelectIndex(size_t index) {
		selectedIndex_ = index;
		if (selectedIndex_ < topIndex_) {
			topIndex_ = selectedIndex_;
		}
		if (selectedIndex_ >= topIndex_ + VisibleRows()) {
			topIndex_ = selectedIndex_ - VisibleRows() + 1;
		}
		if (onChanged_) {
			onChanged_(items_[selectedIndex_]);
		}
	}

	std::vector<std::wstring> items_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	size_t selectedIndex_ = 0;
	size_t topIndex_ = 0;
	std::optional<size_t> hoveredRow_;
	std::function<void(std::wstring_view)> onChanged_;
};

class ComboBox final : public UIComponent {
public:
	ComboBox(std::vector<std::wstring> items,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> outline,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		std::function<void(std::wstring_view)> onChanged)
		: items_(std::move(items)), format_(std::move(format)), surface_(std::move(surface)), accent_(std::move(accent)), outline_(std::move(outline)), textBrush_(std::move(textBrush)), onChanged_(std::move(onChanged)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	bool HitTest(ID2D1Factory1*, D2D1_POINT_2F point) const override {
		return PointInRect(bounds_, point) || (open_ && PointInRect(PopupBounds(), point));
	}

	void Render(ID2D1DeviceContext* context) override {
		const float openAmount = openAnimation_.Value();
		context->FillRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), surface_.Get());
		outline_->SetOpacity(0.5f + 0.5f * (std::max)(FocusProgress(), openAmount));
		context->DrawRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), outline_.Get(), 1.0f + (std::max)(FocusProgress(), openAmount));
		if (!items_.empty()) {
			const std::wstring& selected = items_[selectedIndex_];
			context->DrawTextW(selected.c_str(), static_cast<UINT32>(selected.size()), format_.Get(), D2D1::RectF(bounds_.left + 12.0f, bounds_.top, bounds_.right - 28.0f, bounds_.bottom), textBrush_.Get());
		}
		context->DrawLine(D2D1::Point2F(bounds_.right - 18.0f, bounds_.top + 14.0f), D2D1::Point2F(bounds_.right - 12.0f, bounds_.top + 22.0f), outline_.Get(), 1.8f);
		context->DrawLine(D2D1::Point2F(bounds_.right - 12.0f, bounds_.top + 22.0f), D2D1::Point2F(bounds_.right - 6.0f, bounds_.top + 14.0f), outline_.Get(), 1.8f);
		if (openAmount <= 0.01f) {
			return;
		}
		D2D1_RECT_F popup = PopupBounds(openAmount);
		context->FillRoundedRectangle(D2D1::RoundedRect(popup, 12.0f, 12.0f), surface_.Get());
		context->DrawRoundedRectangle(D2D1::RoundedRect(popup, 12.0f, 12.0f), outline_.Get(), 1.5f);
		for (size_t index = 0; index < items_.size(); ++index) {
			D2D1_RECT_F rowRect = D2D1::RectF(popup.left + 8.0f, popup.top + 6.0f + index * 22.0f, popup.right - 8.0f, popup.top + 26.0f + index * 22.0f);
			if (hoveredIndex_ && *hoveredIndex_ == index) {
				accent_->SetOpacity(0.18f);
				context->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), accent_.Get());
			}
			context->DrawTextW(items_[index].c_str(), static_cast<UINT32>(items_[index].size()), format_.Get(), rowRect, textBrush_.Get());
		}
	}

	void OnPointerMove(D2D1_POINT_2F point) override {
		if (!open_) {
			return;
		}
		hoveredIndex_ = PopupRowFromPoint(point);
	}

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(openAnimation_, open_ ? 1.0f : 0.0f);
		}
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); }

	void OnPointerUp(D2D1_POINT_2F point) override {
		if (!pressed_) {
			return;
		}
		UIComponent::OnPointerUp(point);
		if (PointInRect(bounds_, point)) {
			open_ = !open_;
			Animate(openAnimation_, open_ ? 1.0f : 0.0f, 0.14);
			return;
		}
		if (open_) {
			auto row = PopupRowFromPoint(point);
			if (row && *row < items_.size()) {
				selectedIndex_ = *row;
				if (onChanged_) {
					onChanged_(items_[selectedIndex_]);
				}
			}
			open_ = false;
			Animate(openAnimation_, 0.0f, 0.14);
		}
	}

	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (items_.empty()) {
			return;
		}
		if (key == VK_SPACE || key == VK_RETURN) {
			open_ = !open_;
			Animate(openAnimation_, open_ ? 1.0f : 0.0f, 0.14);
			return;
		}
		if (key == VK_ESCAPE) {
			open_ = false;
			Animate(openAnimation_, 0.0f, 0.14);
			return;
		}
		if (key == VK_UP && selectedIndex_ > 0) {
			--selectedIndex_;
		}
		if (key == VK_DOWN && selectedIndex_ + 1 < items_.size()) {
			++selectedIndex_;
		}
		if (onChanged_) {
			onChanged_(items_[selectedIndex_]);
		}
	}

private:
	D2D1_RECT_F PopupBounds(float openAmount = 1.0f) const {
		const float fullHeight = static_cast<float>(items_.size()) * 22.0f + 8.0f;
		return D2D1::RectF(bounds_.left, bounds_.bottom + 6.0f, bounds_.right, bounds_.bottom + 6.0f + fullHeight * openAmount);
	}

	std::optional<size_t> PopupRowFromPoint(D2D1_POINT_2F point) const {
		D2D1_RECT_F popup = PopupBounds();
		if (!PointInRect(popup, point)) {
			return std::nullopt;
		}
		const float relativeY = point.y - popup.top - 6.0f;
		if (relativeY < 0.0f) {
			return std::nullopt;
		}
		size_t row = static_cast<size_t>(relativeY / 22.0f);
		return row < items_.size() ? std::optional<size_t>(row) : std::nullopt;
	}

	std::vector<std::wstring> items_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	size_t selectedIndex_ = 0;
	bool open_ = false;
	UIAnimation openAnimation_{};
	std::optional<size_t> hoveredIndex_;
	std::function<void(std::wstring_view)> onChanged_;
};

enum class ScrollOrientation {
	Horizontal,
	Vertical,
};

class ScrollBar final : public UIComponent {
public:
	ScrollBar(ScrollOrientation orientation,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> outline,
		float value,
		float pageSize,
		std::function<void(float)> onChanged)
		: orientation_(orientation), surface_(std::move(surface)), accent_(std::move(accent)), outline_(std::move(outline)), value_(Clamp01(value)), pageSize_(Clamp01(pageSize)), onChanged_(std::move(onChanged)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(valueAnimation_, value_);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		context->FillRoundedRectangle(D2D1::RoundedRect(bounds_, 10.0f, 10.0f), surface_.Get());
		outline_->SetOpacity(0.5f + 0.5f * FocusProgress());
		context->DrawRoundedRectangle(D2D1::RoundedRect(bounds_, 10.0f, 10.0f), outline_.Get(), 1.0f + FocusProgress());
		D2D1_RECT_F thumb = ThumbBounds();
		accent_->SetOpacity(0.62f + 0.18f * HoverProgress() + 0.15f * PressProgress());
		context->FillRoundedRectangle(D2D1::RoundedRect(thumb, 8.0f, 8.0f), accent_.Get());
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); UpdateValue(point); }
	void OnPointerMove(D2D1_POINT_2F point) override { if (pressed_) UpdateValue(point); }
	void OnPointerUp(D2D1_POINT_2F point) override { if (pressed_) UpdateValue(point); UIComponent::OnPointerUp(point); }
	void OnMouseWheel(int delta) override { SetValue(value_ + (delta > 0 ? -0.05f : 0.05f)); }
	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_LEFT || key == VK_UP) SetValue(value_ - 0.03f);
		if (key == VK_RIGHT || key == VK_DOWN) SetValue(value_ + 0.03f);
	}

private:
	D2D1_RECT_F ThumbBounds() const {
		const float value = valueAnimation_.Value();
		if (orientation_ == ScrollOrientation::Horizontal) {
			const float width = (bounds_.right - bounds_.left) * (std::max)(pageSize_, 0.15f);
			const float left = bounds_.left + (bounds_.right - bounds_.left - width) * value;
			return D2D1::RectF(left, bounds_.top + 2.0f, left + width, bounds_.bottom - 2.0f);
		}
		const float height = (bounds_.bottom - bounds_.top) * (std::max)(pageSize_, 0.15f);
		const float top = bounds_.top + (bounds_.bottom - bounds_.top - height) * value;
		return D2D1::RectF(bounds_.left + 2.0f, top, bounds_.right - 2.0f, top + height);
	}

	void UpdateValue(D2D1_POINT_2F point) {
		if (orientation_ == ScrollOrientation::Horizontal) {
			float width = (std::max)(1.0f, bounds_.right - bounds_.left);
			SetValue((point.x - bounds_.left) / width);
			return;
		}
		float height = (std::max)(1.0f, bounds_.bottom - bounds_.top);
		SetValue((point.y - bounds_.top) / height);
	}

	void SetValue(float value) {
		value_ = Clamp01(value);
		Animate(valueAnimation_, value_, 0.12);
		if (onChanged_) onChanged_(value_);
	}

	ScrollOrientation orientation_ = ScrollOrientation::Horizontal;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	float value_ = 0.0f;
	float pageSize_ = 0.2f;
	UIAnimation valueAnimation_{};
	std::function<void(float)> onChanged_;
};

class Knob final : public UIComponent {
public:
	Knob(std::wstring label,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<ID2D1SolidColorBrush> surface,
		ComPtr<ID2D1SolidColorBrush> accent,
		ComPtr<ID2D1SolidColorBrush> outline,
		ComPtr<ID2D1SolidColorBrush> textBrush,
		float value,
		std::function<void(float)> onChanged)
		: label_(std::move(label)), format_(std::move(format)), surface_(std::move(surface)), accent_(std::move(accent)), outline_(std::move(outline)), textBrush_(std::move(textBrush)), value_(Clamp01(value)), onChanged_(std::move(onChanged)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(valueAnimation_, value_);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		context->DrawTextW(label_.c_str(), static_cast<UINT32>(label_.size()), format_.Get(), D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f), textBrush_.Get());
		D2D1_POINT_2F center{ (bounds_.left + bounds_.right) * 0.5f, bounds_.top + 70.0f };
		const float radius = 36.0f;
		context->FillEllipse(D2D1::Ellipse(center, radius, radius), surface_.Get());
		outline_->SetOpacity(0.6f + 0.4f * FocusProgress());
		context->DrawEllipse(D2D1::Ellipse(center, radius, radius), outline_.Get(), 1.0f + FocusProgress());
		const float value = valueAnimation_.Value();
		const float angle = 3.1415926f * (1.25f + value * 1.5f);
		D2D1_POINT_2F handle{ center.x + std::cos(angle) * 26.0f, center.y + std::sin(angle) * 26.0f };
		accent_->SetOpacity(0.7f + 0.15f * HoverProgress() + 0.15f * PressProgress());
		context->DrawLine(center, handle, accent_.Get(), 3.0f);
		std::wstringstream ss;
		ss << static_cast<int>(value * 100.0f);
		auto valueText = ss.str();
		context->DrawTextW(valueText.c_str(), static_cast<UINT32>(valueText.size()), format_.Get(), D2D1::RectF(bounds_.left, center.y + 42.0f, bounds_.right, bounds_.bottom), textBrush_.Get());
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); anchor_ = point; anchorValue_ = value_; }
	void OnPointerMove(D2D1_POINT_2F point) override {
		if (!pressed_) return;
		const float delta = (anchor_.x - point.x + point.y - anchor_.y) * 0.005f;
		SetValue(anchorValue_ + delta);
	}
	void OnPointerUp(D2D1_POINT_2F point) override { UIComponent::OnPointerUp(point); }
	void OnMouseWheel(int delta) override { SetValue(value_ + (delta > 0 ? 0.04f : -0.04f)); }
	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_LEFT || key == VK_DOWN) SetValue(value_ - 0.03f);
		if (key == VK_RIGHT || key == VK_UP) SetValue(value_ + 0.03f);
	}

private:
	void SetValue(float value) {
		value_ = Clamp01(value);
		Animate(valueAnimation_, value_, 0.12);
		if (onChanged_) onChanged_(value_);
	}

	std::wstring label_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<ID2D1SolidColorBrush> surface_;
	ComPtr<ID2D1SolidColorBrush> accent_;
	ComPtr<ID2D1SolidColorBrush> outline_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	float value_ = 0.0f;
	UIAnimation valueAnimation_{};
	D2D1_POINT_2F anchor_{ 0.0f, 0.0f };
	float anchorValue_ = 0.0f;
	std::function<void(float)> onChanged_;
};

inline D2D1_RECT_F UnionRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
	return D2D1::RectF((std::min)(a.left, b.left), (std::min)(a.top, b.top), (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom));
}

} // namespace detail

class DemoUiHost {
public:
	struct KDNode {
		D2D1_RECT_F bounds{ D2D1::RectF() };
		int axis = 0;
		const UIComponent* item = nullptr;
		std::unique_ptr<KDNode> left;
		std::unique_ptr<KDNode> right;
	};

	DemoUiHost(GraphicsContext graphics, HostCallbacks callbacks)
		: graphics_(std::move(graphics)), callbacks_(std::move(callbacks)) {
		animationSystem_.Initialize();
		InitializeResources();
		BuildDemoScene();
	}

	CursorKind CurrentCursor() const {
		return currentCursor_;
	}

	D2D1_RECT_F VisibleUiBounds() const {
		return visibleUiBounds_;
	}

	void SetViewport(const D2D1_RECT_F& viewport, float dpiScale) {
		if (viewport.left != viewport_.left || viewport.top != viewport_.top || viewport.right != viewport_.right || viewport.bottom != viewport_.bottom || dpiScale != dpiScale_) {
			viewport_ = viewport;
			dpiScale_ = dpiScale;
			layoutDirty_ = true;
			staticLayerDirty_ = true;
			dynamicDirty_ = true;
		}
	}

	void HandleWin32Message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		if (!graphics_.d2dFactory || components_.empty()) {
			return;
		}
		D2D1_POINT_2F point = D2D1::Point2F();
		const bool hasMousePoint = msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_MOUSEWHEEL;
		if (msg == WM_MOUSEWHEEL) {
			POINT screenPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			ScreenToClient(hwnd, &screenPoint);
			point = D2D1::Point2F(static_cast<float>(screenPoint.x) / dpiScale_, static_cast<float>(screenPoint.y) / dpiScale_);
		}
		else if (hasMousePoint) {
			point = D2D1::Point2F(static_cast<float>(GET_X_LPARAM(lParam)) / dpiScale_, static_cast<float>(GET_Y_LPARAM(lParam)) / dpiScale_);
		}

		switch (msg) {
		case WM_MOUSEMOVE: {
			if (!PointInViewport(point)) {
				ClearHover();
				currentCursor_ = CursorKind::None;
				dynamicDirty_ = true;
				return;
			}
			UIComponent* target = captured_ ? captured_ : Pick(point);
			SetHovered(target);
			currentCursor_ = target ? target->Cursor() : CursorKind::Arrow;
			if (captured_) {
				captured_->OnPointerMove(point);
			}
			dynamicDirty_ = true;
			return;
		}
		case WM_LBUTTONDOWN: {
			if (!PointInViewport(point)) {
				SetFocused(nullptr);
				currentCursor_ = CursorKind::None;
				return;
			}
			UIComponent* target = Pick(point);
			SetFocused(target && target->IsFocusable() ? target : nullptr);
			captured_ = target;
			if (target) {
				target->OnPointerDown(point);
				dynamicDirty_ = true;
			}
			return;
		}
		case WM_LBUTTONUP: {
			if (captured_) {
				captured_->OnPointerUp(point);
				captured_ = nullptr;
				dynamicDirty_ = true;
			}
			return;
		}
		case WM_MOUSELEAVE: {
			ClearHover();
			dynamicDirty_ = true;
			return;
		}
		case WM_MOUSEWHEEL: {
			UIComponent* target = hovered_ ? hovered_ : focused_;
			if (target) {
				target->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
				dynamicDirty_ = true;
			}
			return;
		}
		case WM_CHAR: {
			if (focused_) {
				focused_->OnChar(static_cast<wchar_t>(wParam));
				dynamicDirty_ = true;
			}
			return;
		}
		case WM_IME_STARTCOMPOSITION: {
			if (focused_) {
				focused_->OnImeStart(hwnd);
				dynamicDirty_ = true;
			}
			return;
		}
		case WM_IME_COMPOSITION: {
			if (focused_) {
				focused_->OnImeComposition(hwnd, lParam);
				dynamicDirty_ = true;
			}
			return;
		}
		case WM_IME_ENDCOMPOSITION: {
			if (focused_) {
				focused_->OnImeEnd(hwnd);
				dynamicDirty_ = true;
			}
			return;
		}
		default:
			break;
		}
	}

	bool HandleKeyDown(WPARAM key, const KeyModifiers& modifiers) {
		if (focusOrder_.empty()) {
			return false;
		}
		if (key == VK_TAB) {
			auto it = std::ranges::find(focusOrder_, focused_);
			if (it == focusOrder_.end()) {
				SetFocused(focusOrder_.front());
			}
			else {
				++it;
				if (it == focusOrder_.end()) {
					it = focusOrder_.begin();
				}
				SetFocused(*it);
			}
			dynamicDirty_ = true;
			return true;
		}
		if (focused_) {
			focused_->OnKeyDown(key, modifiers);
			dynamicDirty_ = true;
			return true;
		}
		return false;
	}

	void SetAnimationProgress(float normalizedProgress) {
		progressAnimation_.SetObservedValue((std::clamp)(normalizedProgress, 0.0f, 1.0f));
		dynamicDirty_ = true;
	}

	void Render(ID2D1DeviceContext* context = nullptr) {
		auto* targetContext = context ? context : graphics_.d2dContext.Get();
		if (!targetContext) {
			return;
		}
		animationSystem_.Update();
		if (layoutDirty_) {
			Arrange();
		}
		if (rightCard_ && rightCard_->Visible()) {
			ComPtr<ID2D1SolidColorBrush> localPanel;
			ComPtr<ID2D1SolidColorBrush> localOutline;
			targetContext->CreateSolidColorBrush(D2D1::ColorF(0.985f, 0.99f, 0.998f, 0.98f), &localPanel);
			targetContext->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.42f, 0.78f, 0.35f), &localOutline);
			const auto rounded = D2D1::RoundedRect(rightCard_->Bounds(), detail::kCardRadius, detail::kCardRadius);
			if (localPanel) {
				targetContext->FillRoundedRectangle(rounded, localPanel.Get());
			}
			if (localOutline) {
				targetContext->DrawRoundedRectangle(rounded, localOutline.Get(), 1.0f);
			}
		}
		if (rightCard_ && rightCard_->Visible()) {
			rightCard_->Render(targetContext);
			for (auto* item : rightLayoutOrder_) {
				if (item && item->Visible()) {
					item->Render(targetContext);
				}
			}
		}
		if (leftCard_ && leftCard_->Visible()) {
			leftCard_->Render(targetContext);
			for (auto* item : leftLayoutOrder_) {
				if (item && item->Visible()) {
					item->Render(targetContext);
				}
			}
		}
		dynamicDirty_ = false;
	}

private:
	using TextBlock = detail::TextBlock;
	using CardSurface = detail::CardSurface;
	using Button = detail::Button;
	using ImageFrame = detail::ImageFrame;
	using ImageButton = detail::ImageButton;
	using Checkbox = detail::Checkbox;
	using RadioButton = detail::RadioButton;
	using Slider = detail::Slider;
	using ProgressBar = detail::ProgressBar;
	using TextInput = detail::TextInput;
	using ListBox = detail::ListBox;
	using ComboBox = detail::ComboBox;
	using ScrollBar = detail::ScrollBar;
	using Knob = detail::Knob;
	using ScrollOrientation = detail::ScrollOrientation;

	void InitializeResources() {
		if (!graphics_.d2dContext || !graphics_.dwriteFactory) {
			return;
		}
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(0.95f, 0.96f, 0.98f, 0.95f), &panelBrush_);
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(0.78f, 0.82f, 0.9f, 0.65f), &outlineBrush_);
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(0.14f, 0.46f, 0.82f), &accentBrush_);
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(0.14f, 0.46f, 0.82f, 0.18f), &accentSoftBrush_);
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(0.08f, 0.12f, 0.18f), &textBrush_);
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(0.32f, 0.36f, 0.44f), &mutedTextBrush_);
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(1.0f, 1.0f, 1.0f, 0.84f), &surfaceBrush_);
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(0.92f, 0.94f, 0.98f, 0.92f), &surfaceAltBrush_);
		graphics_.d2dContext->CreateSolidColorBrush(detail::MakeColor(0.12f, 0.64f, 0.42f), &successBrush_);

		graphics_.dwriteFactory->CreateTextFormat(L"Segoe UI Semibold", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"en-us", &titleFormat_);
		graphics_.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &bodyFormat_);
		graphics_.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &captionFormat_);
		if (titleFormat_) titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
		if (bodyFormat_) bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		if (captionFormat_) captionFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
	}

	void BuildDemoScene() {
		layout_ = std::make_unique<VerticalStackLayout>(24.0f, 12.0f);
		components_.clear();
		leftLayoutOrder_.clear();
		rightLayoutOrder_.clear();
		focusOrder_.clear();

		auto selectedAimStyle = std::make_shared<int>(0);

		auto leftCard = std::make_unique<CardSurface>(panelBrush_, outlineBrush_);
		leftCard->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth, 540.0f));
		leftCard->SetZIndex(0);
		leftCard_ = leftCard.get();
		components_.push_back(std::move(leftCard));

		auto rightCard = std::make_unique<CardSurface>(panelBrush_, outlineBrush_);
		rightCard->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth, 540.0f));
		rightCard->SetZIndex(0);
		rightCard_ = rightCard.get();
		components_.push_back(std::move(rightCard));

		auto title = std::make_unique<TextBlock>(L"Native UI Host", titleFormat_, textBrush_);
		title->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 34.0f));
		title->SetZIndex(1);
		leftLayoutOrder_.push_back(title.get());
		components_.push_back(std::move(title));

		auto subtitle = std::make_unique<TextBlock>(L"Header-only host with KD-tree picking, static-layer cache, text input and composite widgets.", captionFormat_, mutedTextBrush_);
		subtitle->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 32.0f));
		subtitle->SetZIndex(1);
		leftLayoutOrder_.push_back(subtitle.get());
		components_.push_back(std::move(subtitle));

		auto preview = std::make_unique<ImageFrame>(outlineBrush_, accentBrush_, surfaceAltBrush_);
		preview->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 110.0f));
		preview->SetZIndex(1);
		leftLayoutOrder_.push_back(preview.get());
		components_.push_back(std::move(preview));

		auto previewButton = std::make_unique<ImageButton>(L"Preview Asset", bodyFormat_, surfaceBrush_, accentBrush_, outlineBrush_, textBrush_, [this]() {
			SetStatus(L"Image button activated from the header-only UI host.");
		});
		previewButton->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 46.0f));
		previewButton->SetZIndex(3);
		focusOrder_.push_back(previewButton.get());
		leftLayoutOrder_.push_back(previewButton.get());
		components_.push_back(std::move(previewButton));

		auto resetButton = std::make_unique<Button>(L"Reset Canvas", bodyFormat_, accentBrush_, surfaceBrush_, outlineBrush_, surfaceBrush_, [this]() {
			if (callbacks_.onResetCanvas) {
				callbacks_.onResetCanvas();
			}
			SetStatus(L"Reset requested from the native UI layer.");
		}, true);
		resetButton->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		resetButton->SetZIndex(3);
		focusOrder_.push_back(resetButton.get());
		leftLayoutOrder_.push_back(resetButton.get());
		components_.push_back(std::move(resetButton));

		auto checkbox = std::make_unique<Checkbox>(L"Mirror show-grid state", bodyFormat_, surfaceBrush_, successBrush_, textBrush_, outlineBrush_, true, [this](bool checked) {
			if (callbacks_.onGridChanged) {
				callbacks_.onGridChanged(checked);
			}
			SetStatus(checked ? L"Grid enabled from checkbox." : L"Grid disabled from checkbox.");
		});
		checkbox->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		checkbox->SetZIndex(3);
		focusOrder_.push_back(checkbox.get());
		leftLayoutOrder_.push_back(checkbox.get());
		components_.push_back(std::move(checkbox));

		auto radio0 = std::make_unique<RadioButton>(L"Aim style 0: ring", bodyFormat_, surfaceBrush_, accentBrush_, textBrush_, outlineBrush_, selectedAimStyle, 0, [this](int value) {
			if (callbacks_.onAimStyleChanged) callbacks_.onAimStyleChanged(value);
			SetStatus(L"Aim style switched to ring mode.");
		});
		radio0->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		radio0->SetZIndex(3);
		focusOrder_.push_back(radio0.get());
		leftLayoutOrder_.push_back(radio0.get());
		components_.push_back(std::move(radio0));

		auto radio1 = std::make_unique<RadioButton>(L"Aim style 1: dot", bodyFormat_, surfaceBrush_, accentBrush_, textBrush_, outlineBrush_, selectedAimStyle, 1, [this](int value) {
			if (callbacks_.onAimStyleChanged) callbacks_.onAimStyleChanged(value);
			SetStatus(L"Aim style switched to dot mode.");
		});
		radio1->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		radio1->SetZIndex(3);
		focusOrder_.push_back(radio1.get());
		leftLayoutOrder_.push_back(radio1.get());
		components_.push_back(std::move(radio1));

		auto radio2 = std::make_unique<RadioButton>(L"Aim style 2: triangle", bodyFormat_, surfaceBrush_, accentBrush_, textBrush_, outlineBrush_, selectedAimStyle, 2, [this](int value) {
			if (callbacks_.onAimStyleChanged) callbacks_.onAimStyleChanged(value);
			SetStatus(L"Aim style switched to triangle mode.");
		});
		radio2->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		radio2->SetZIndex(3);
		focusOrder_.push_back(radio2.get());
		leftLayoutOrder_.push_back(radio2.get());
		components_.push_back(std::move(radio2));

		auto slider = std::make_unique<Slider>(L"Aim radius", captionFormat_, surfaceAltBrush_, accentBrush_, textBrush_, outlineBrush_, 0.2f, [this](float value) {
			if (callbacks_.onAimRadiusChanged) callbacks_.onAimRadiusChanged(5.0f + value * 75.0f);
			std::wstringstream ss;
			ss << L"Aim radius updated to " << static_cast<int>(5.0f + value * 75.0f) << L" px.";
			SetStatus(ss.str());
		});
		slider->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 40.0f));
		slider->SetZIndex(3);
		focusOrder_.push_back(slider.get());
		leftLayoutOrder_.push_back(slider.get());
		components_.push_back(std::move(slider));

		auto progress = std::make_unique<ProgressBar>(L"UIAnimation bridge", captionFormat_, surfaceAltBrush_, accentBrush_, textBrush_, &progressAnimation_);
		progress->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 40.0f));
		progress->SetZIndex(3);
		leftLayoutOrder_.push_back(progress.get());
		components_.push_back(std::move(progress));

		auto singleInput = std::make_unique<TextInput>(L"Single-line input", L"Type a command...", L"draw hitmarker", graphics_.dwriteFactory, bodyFormat_, surfaceBrush_, outlineBrush_, textBrush_, mutedTextBrush_, accentSoftBrush_, false, [this](std::wstring_view value) {
			std::wstring status = L"Single-line input: ";
			status.append(value);
			SetStatus(status);
		});
		singleInput->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 60.0f));
		singleInput->SetZIndex(3);
		singleInput_ = singleInput.get();
		focusOrder_.push_back(singleInput_);
		rightLayoutOrder_.push_back(singleInput_);
		components_.push_back(std::move(singleInput));

		auto multiInput = std::make_unique<TextInput>(L"Multiline editor", L"Notes...", L"Header-only migration complete.\nNext: richer text selection and IME support.", graphics_.dwriteFactory, bodyFormat_, surfaceBrush_, outlineBrush_, textBrush_, mutedTextBrush_, accentSoftBrush_, true, [this](std::wstring_view) {
			SetStatus(L"Multiline editor changed.");
		});
		multiInput->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 118.0f));
		multiInput->SetZIndex(3);
		multiInput_ = multiInput.get();
		focusOrder_.push_back(multiInput_);
		rightLayoutOrder_.push_back(multiInput_);
		components_.push_back(std::move(multiInput));

		auto listBox = std::make_unique<ListBox>(std::vector<std::wstring>{ L"Aster", L"Beryl", L"Cinder", L"Delta", L"Ember", L"Flint", L"Grove", L"Halo" }, bodyFormat_, surfaceBrush_, accentBrush_, outlineBrush_, textBrush_, [this](std::wstring_view value) {
			std::wstring status = L"List box selected: ";
			status.append(value);
			SetStatus(status);
		});
		listBox->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 142.0f));
		listBox->SetZIndex(3);
		listBox_ = listBox.get();
		focusOrder_.push_back(listBox_);
		rightLayoutOrder_.push_back(listBox_);
		components_.push_back(std::move(listBox));

		auto comboBox = std::make_unique<ComboBox>(std::vector<std::wstring>{ L"Telemetry", L"Diagnostics", L"Staging", L"Release" }, bodyFormat_, surfaceBrush_, accentBrush_, outlineBrush_, textBrush_, [this](std::wstring_view value) {
			std::wstring status = L"Combo box selected: ";
			status.append(value);
			SetStatus(status);
		});
		comboBox->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 38.0f));
		comboBox->SetZIndex(10);
		comboBox_ = comboBox.get();
		focusOrder_.push_back(comboBox_);
		rightLayoutOrder_.push_back(comboBox_);
		components_.push_back(std::move(comboBox));

		auto horizontalScroll = std::make_unique<ScrollBar>(ScrollOrientation::Horizontal, surfaceAltBrush_, accentBrush_, outlineBrush_, 0.15f, 0.25f, [this](float value) {
			std::wstringstream ss;
			ss << L"Horizontal scrollbar: " << static_cast<int>(value * 100.0f) << L"%";
			SetStatus(ss.str());
		});
		horizontalScroll->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 22.0f));
		horizontalScroll->SetZIndex(3);
		horizontalScrollBar_ = horizontalScroll.get();
		focusOrder_.push_back(horizontalScrollBar_);
		rightLayoutOrder_.push_back(horizontalScrollBar_);
		components_.push_back(std::move(horizontalScroll));

		auto verticalScroll = std::make_unique<ScrollBar>(ScrollOrientation::Vertical, surfaceAltBrush_, accentBrush_, outlineBrush_, 0.0f, 0.3f, [this](float value) {
			if (listBox_) {
				listBox_->SetScrollNormalized(value);
			}
			std::wstringstream ss;
			ss << L"Vertical scrollbar: " << static_cast<int>(value * 100.0f) << L"%";
			SetStatus(ss.str());
		});
		verticalScroll->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 92.0f));
		verticalScroll->SetZIndex(3);
		verticalScrollBar_ = verticalScroll.get();
		focusOrder_.push_back(verticalScrollBar_);
		rightLayoutOrder_.push_back(verticalScrollBar_);
		components_.push_back(std::move(verticalScroll));

		auto knob = std::make_unique<Knob>(L"Encoder", captionFormat_, surfaceAltBrush_, accentBrush_, outlineBrush_, textBrush_, 0.42f, [this](float value) {
			std::wstringstream ss;
			ss << L"Knob rotated to " << static_cast<int>(value * 100.0f) << L"%.";
			SetStatus(ss.str());
		});
		knob->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 136.0f));
		knob->SetZIndex(3);
		knob_ = knob.get();
		focusOrder_.push_back(knob_);
		rightLayoutOrder_.push_back(knob_);
		components_.push_back(std::move(knob));

		auto statusText = std::make_unique<TextBlock>(L"Bridge ready: header-only host, text input, list and combo controls are live.", captionFormat_, mutedTextBrush_, true);
		statusText->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 56.0f));
		statusText->SetZIndex(3);
		statusText_ = statusText.get();
		rightLayoutOrder_.push_back(statusText_);
		components_.push_back(std::move(statusText));

		for (const auto& component : components_) {
			component->AttachAnimationSystem(&animationSystem_);
		}
	}

	void Arrange() {
		if (!leftCard_ || !rightCard_) {
			return;
		}
		const float gap = 24.0f;
		const float availableWidth = viewport_.right - viewport_.left - 48.0f;
		const bool twoColumn = availableWidth >= (detail::kCardWidth * 2.0f + gap + 16.0f);
		const float top = viewport_.top + 24.0f;
		const float bottom = viewport_.bottom - 24.0f;
		if (twoColumn) {
			leftCard_->SetVisible(true);
			rightCard_->SetVisible(true);
			for (auto* item : leftLayoutOrder_) {
				item->SetVisible(true);
			}
			for (auto* item : rightLayoutOrder_) {
				item->SetVisible(true);
			}
			const float cardWidth = (availableWidth - gap) * 0.5f;
			const D2D1_RECT_F leftCardBounds = D2D1::RectF(viewport_.left + 24.0f, top, viewport_.left + 24.0f + cardWidth, bottom);
			const D2D1_RECT_F rightCardBounds = D2D1::RectF(leftCardBounds.right + gap, top, leftCardBounds.right + gap + cardWidth, bottom);
			leftCard_->SetBounds(leftCardBounds);
			rightCard_->SetBounds(rightCardBounds);
			layout_->Arrange(leftLayoutOrder_, leftCardBounds, dpiScale_);
			layout_->Arrange(rightLayoutOrder_, rightCardBounds, dpiScale_);
		}
		else {
			leftCard_->SetVisible(false);
			rightCard_->SetVisible(true);
			for (auto* item : leftLayoutOrder_) {
				item->SetVisible(false);
			}
			for (auto* item : rightLayoutOrder_) {
				item->SetVisible(true);
			}
			const float cardWidth = (std::min)(detail::kCardWidth, (std::max)(300.0f, availableWidth));
			const D2D1_RECT_F rightCardBounds = D2D1::RectF(viewport_.right - cardWidth - 24.0f, top, viewport_.right - 24.0f, bottom);
			rightCard_->SetBounds(rightCardBounds);
			layout_->Arrange(rightLayoutOrder_, rightCardBounds, dpiScale_);
		}

		visibleUiBounds_ = D2D1::RectF();
		if (leftCard_->Visible()) {
			visibleUiBounds_ = leftCard_->Bounds();
		}
		if (rightCard_->Visible()) {
			visibleUiBounds_ = visibleUiBounds_.right > visibleUiBounds_.left ? detail::UnionRect(visibleUiBounds_, rightCard_->Bounds()) : rightCard_->Bounds();
		}

		RebuildHitIndex();
		layoutDirty_ = false;
		staticLayerDirty_ = true;
	}

	void RebuildStaticLayer() {
		if (!graphics_.d2dContext) {
			return;
		}
		ComPtr<ID2D1Image> previousTarget;
		graphics_.d2dContext->GetTarget(&previousTarget);
		staticLayer_.Reset();
		graphics_.d2dContext->CreateCommandList(&staticLayer_);
		if (!staticLayer_) {
			return;
		}
		graphics_.d2dContext->SetTarget(staticLayer_.Get());
		graphics_.d2dContext->BeginDraw();
		for (const auto& component : components_) {
			if (component->Visible() && !component->IsDynamic()) {
				component->Render(graphics_.d2dContext.Get());
			}
		}
		graphics_.d2dContext->EndDraw();
		staticLayer_->Close();
		graphics_.d2dContext->SetTarget(previousTarget.Get());
		staticLayerDirty_ = false;
	}

	void RebuildHitIndex() {
		std::vector<const UIComponent*> hittable;
		for (const auto& component : components_) {
			if (component->Visible() && component->Enabled() && component->ZIndex() > 0) {
				hittable.push_back(component.get());
			}
		}
		hitIndex_ = BuildKDTree(hittable, 0);
	}

	void ClearHover() {
		if (hovered_) {
			hovered_->OnHover(false);
			hovered_ = nullptr;
		}
	}

	void SetHovered(UIComponent* component) {
		if (hovered_ == component) {
			return;
		}
		if (hovered_) {
			hovered_->OnHover(false);
		}
		hovered_ = component;
		if (hovered_) {
			hovered_->OnHover(true);
		}
	}

	void SetFocused(UIComponent* component) {
		if (focused_ == component) {
			return;
		}
		if (focused_) {
			focused_->OnFocus(false);
		}
		focused_ = component;
		if (focused_) {
			focused_->OnFocus(true);
		}
	}

	UIComponent* Pick(D2D1_POINT_2F point) const {
		auto candidates = CollectCandidates(point);
		std::vector<UIComponent*> hits;
		for (const auto* candidate : candidates) {
			auto* mutableCandidate = const_cast<UIComponent*>(candidate);
			if (mutableCandidate->HitTest(graphics_.d2dFactory.Get(), point)) {
				hits.push_back(mutableCandidate);
			}
		}
		if (hits.empty()) {
			return nullptr;
		}
		std::ranges::sort(hits, [](const UIComponent* lhs, const UIComponent* rhs) {
			return lhs->ZIndex() > rhs->ZIndex();
		});
		return hits.front();
	}

	std::vector<const UIComponent*> CollectCandidates(D2D1_POINT_2F point) const {
		std::vector<const UIComponent*> candidates;
		QueryKDTree(hitIndex_.get(), point, candidates);
		return candidates;
	}

	bool PointInViewport(D2D1_POINT_2F point) const {
		return UIComponent::PointInRect(viewport_, point);
	}

	void SetStatus(std::wstring text) {
		if (statusText_) {
			statusText_->SetText(std::move(text));
		}
	}

	static std::unique_ptr<KDNode> BuildKDTree(std::vector<const UIComponent*>& items, int depth) {
		if (items.empty()) {
			return nullptr;
		}
		const int axis = depth % 2;
		const auto midpoint = items.begin() + static_cast<std::ptrdiff_t>(items.size() / 2);
		std::nth_element(items.begin(), midpoint, items.end(), [axis](const UIComponent* lhs, const UIComponent* rhs) {
			const auto& leftBounds = lhs->Bounds();
			const auto& rightBounds = rhs->Bounds();
			const float leftCenter = axis == 0 ? (leftBounds.left + leftBounds.right) * 0.5f : (leftBounds.top + leftBounds.bottom) * 0.5f;
			const float rightCenter = axis == 0 ? (rightBounds.left + rightBounds.right) * 0.5f : (rightBounds.top + rightBounds.bottom) * 0.5f;
			if (leftCenter == rightCenter) {
				return lhs->ZIndex() < rhs->ZIndex();
			}
			return leftCenter < rightCenter;
		});

		auto node = std::make_unique<KDNode>();
		node->axis = axis;
		node->item = *midpoint;
		node->bounds = node->item->Bounds();
		std::vector<const UIComponent*> leftItems(items.begin(), midpoint);
		std::vector<const UIComponent*> rightItems(midpoint + 1, items.end());
		node->left = BuildKDTree(leftItems, depth + 1);
		node->right = BuildKDTree(rightItems, depth + 1);
		if (node->left) {
			node->bounds = detail::UnionRect(node->bounds, node->left->bounds);
		}
		if (node->right) {
			node->bounds = detail::UnionRect(node->bounds, node->right->bounds);
		}
		return node;
	}

	static void QueryKDTree(const KDNode* node, D2D1_POINT_2F point, std::vector<const UIComponent*>& out) {
		if (!node || !UIComponent::PointInRect(node->bounds, point)) {
			return;
		}
		if (UIComponent::PointInRect(node->item->Bounds(), point)) {
			out.push_back(node->item);
		}
		QueryKDTree(node->left.get(), point, out);
		QueryKDTree(node->right.get(), point, out);
	}

	GraphicsContext graphics_;
	HostCallbacks callbacks_;
	D2D1_RECT_F viewport_{ D2D1::RectF() };
	float dpiScale_ = 1.0f;
	bool layoutDirty_ = true;
	bool staticLayerDirty_ = true;
	bool dynamicDirty_ = true;

	ComPtr<ID2D1CommandList> staticLayer_;
	ComPtr<ID2D1SolidColorBrush> panelBrush_;
	ComPtr<ID2D1SolidColorBrush> outlineBrush_;
	ComPtr<ID2D1SolidColorBrush> accentBrush_;
	ComPtr<ID2D1SolidColorBrush> accentSoftBrush_;
	ComPtr<ID2D1SolidColorBrush> textBrush_;
	ComPtr<ID2D1SolidColorBrush> mutedTextBrush_;
	ComPtr<ID2D1SolidColorBrush> surfaceBrush_;
	ComPtr<ID2D1SolidColorBrush> surfaceAltBrush_;
	ComPtr<ID2D1SolidColorBrush> successBrush_;
	ComPtr<IDWriteTextFormat> titleFormat_;
	ComPtr<IDWriteTextFormat> bodyFormat_;
	ComPtr<IDWriteTextFormat> captionFormat_;

	std::vector<std::unique_ptr<UIComponent>> components_;
	std::vector<UIComponent*> leftLayoutOrder_;
	std::vector<UIComponent*> rightLayoutOrder_;
	std::vector<UIComponent*> focusOrder_;
	std::unique_ptr<LayoutBase> layout_;
	std::unique_ptr<KDNode> hitIndex_;
	UIComponent* hovered_ = nullptr;
	UIComponent* focused_ = nullptr;
	UIComponent* captured_ = nullptr;
	UIAnimation progressAnimation_;
	UIAnimationSystem animationSystem_;
	CursorKind currentCursor_ = CursorKind::None;
	D2D1_RECT_F visibleUiBounds_{ D2D1::RectF() };

	CardSurface* leftCard_ = nullptr;
	CardSurface* rightCard_ = nullptr;
	TextBlock* statusText_ = nullptr;
	TextInput* singleInput_ = nullptr;
	TextInput* multiInput_ = nullptr;
	ListBox* listBox_ = nullptr;
	ComboBox* comboBox_ = nullptr;
	ScrollBar* horizontalScrollBar_ = nullptr;
	ScrollBar* verticalScrollBar_ = nullptr;
	Knob* knob_ = nullptr;
};

}