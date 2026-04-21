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
#include <unordered_map>
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

enum class HorizontalAlign {
	Left,
	Center,
	Right,
};

enum class VerticalAlign {
	Top,
	Center,
	Bottom,
};

struct TextStyle {
	std::wstring fontFamily = L"Segoe UI";
	float fontSize = 13.0f;
	DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
	DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
	DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
	D2D1_COLOR_F color = D2D1::ColorF(0.09f, 0.09f, 0.11f, 1.0f);
	bool underline = false;
	bool strikethrough = false;
	HorizontalAlign horizontalAlign = HorizontalAlign::Left;
	VerticalAlign verticalAlign = VerticalAlign::Top;
	std::wstring locale = L"en-us";
};

struct StyledTextRun {
	std::wstring text;
	TextStyle style{};
};

struct StyledTextRange {
	size_t start = 0;
	size_t length = 0;
	TextStyle style{};
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

	void AnimateLinear(UIAnimation& animation, float target, double duration = 0.16) {
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
		if (FAILED(library_->CreateLinearTransition(duration, target, &transition)) || !transition) {
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

	bool HasRunningAnimations() const {
		if (!manager_) {
			return false;
		}
		UI_ANIMATION_MANAGER_STATUS status = UI_ANIMATION_MANAGER_IDLE;
		return SUCCEEDED(manager_->GetStatus(&status)) && status != UI_ANIMATION_MANAGER_IDLE;
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
		for (auto& [_, animation] : customAnimations_) {
			animator_->Attach(animation, animation.Value());
		}
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
		if (!visible_) {
			hovered_ = false;
			pressed_ = false;
			focused_ = false;
			Animate(hoverAnimation_, 0.0f, 0.10);
			Animate(pressAnimation_, 0.0f, 0.10);
			Animate(focusAnimation_, 0.0f, 0.10);
		}
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

	virtual bool WantsFrameTick() const {
		return false;
	}

	bool CanFocus() const {
		return visible_ && enabled_ && IsFocusable();
	}

	virtual CursorKind Cursor() const {
		return IsFocusable() ? CursorKind::Hand : CursorKind::Arrow;
	}

	virtual CursorKind CursorAt(D2D1_POINT_2F point) const {
		(void)point;
		return Cursor();
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
	virtual bool OnMouseWheel(int, D2D1_POINT_2F) { return false; }
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

	void RegisterAnimationSlot(std::wstring key, float initialValue = 0.0f) {
		auto [it, inserted] = customAnimations_.try_emplace(std::move(key), UIAnimation(initialValue));
		if (!inserted) {
			it->second.SetObservedValue(initialValue);
		}
		if (animator_) {
			animator_->Attach(it->second, it->second.Value());
		}
	}

	void AnimateSlot(std::wstring_view key, float target, double duration = 0.16, double accel = 0.3, double decel = 0.3) {
		auto animation = customAnimations_.find(std::wstring(key));
		if (animation == customAnimations_.end()) {
			RegisterAnimationSlot(std::wstring(key), target);
			animation = customAnimations_.find(std::wstring(key));
		}
		Animate(animation->second, target, duration, accel, decel);
	}

	void AnimateSlotLinear(std::wstring_view key, float target, double duration = 0.16) {
		auto animation = customAnimations_.find(std::wstring(key));
		if (animation == customAnimations_.end()) {
			RegisterAnimationSlot(std::wstring(key), target);
			animation = customAnimations_.find(std::wstring(key));
		}
		if (animator_) {
			animator_->AnimateLinear(animation->second, target, duration);
		}
		else {
			animation->second.SetObservedValue(target);
		}
	}

	float AnimationSlotValue(std::wstring_view key, float fallback = 0.0f) const {
		auto animation = customAnimations_.find(std::wstring(key));
		return animation == customAnimations_.end() ? fallback : animation->second.Value();
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

	void AnimateLinear(UIAnimation& animation, float target, double duration = 0.16) {
		if (animator_) {
			animator_->AnimateLinear(animation, target, duration);
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
	std::unordered_map<std::wstring, UIAnimation> customAnimations_;
};

class LayoutBase {
public:
	virtual ~LayoutBase() = default;
	virtual float Arrange(std::span<UIComponent*> items, const D2D1_RECT_F& bounds, float dpiScale, float scrollOffset = 0.0f) = 0;
};

class VerticalStackLayout final : public LayoutBase {
public:
	VerticalStackLayout(float padding, float gap) : padding_(padding), gap_(gap) {}

	float Arrange(std::span<UIComponent*> items, const D2D1_RECT_F& bounds, float, float scrollOffset = 0.0f) override {
		float y = bounds.top + padding_ - scrollOffset;
		float contentHeight = padding_;
		bool hasVisibleItems = false;
		for (auto* item : items) {
			if (!item || !item->Visible()) {
				continue;
			}
			if (hasVisibleItems) {
				contentHeight += gap_;
			}
			const float height = item->Bounds().bottom - item->Bounds().top;
			item->SetBounds(D2D1::RectF(bounds.left + padding_, y, bounds.right - padding_, y + height));
			y += height + gap_;
			contentHeight += height;
			hasVisibleItems = true;
		}
		return contentHeight + padding_;
	}

private:
	float padding_ = 0.0f;
	float gap_ = 0.0f;
};

namespace detail {

constexpr float kCardRadius = 14.0f;
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

enum class ScrollOrientation {
	Horizontal,
	Vertical,
};

inline bool NearlyEqual(float lhs, float rhs, float epsilon = 0.25f) {
	return std::fabs(lhs - rhs) <= epsilon;
}

inline bool ColorEqual(const D2D1_COLOR_F& lhs, const D2D1_COLOR_F& rhs) {
	return NearlyEqual(lhs.r, rhs.r, 0.001f) && NearlyEqual(lhs.g, rhs.g, 0.001f) && NearlyEqual(lhs.b, rhs.b, 0.001f) && NearlyEqual(lhs.a, rhs.a, 0.001f);
}

inline bool TextStyleEqual(const TextStyle& lhs, const TextStyle& rhs) {
	return lhs.fontFamily == rhs.fontFamily && NearlyEqual(lhs.fontSize, rhs.fontSize, 0.01f) && lhs.weight == rhs.weight && lhs.style == rhs.style && lhs.stretch == rhs.stretch && ColorEqual(lhs.color, rhs.color) && lhs.underline == rhs.underline && lhs.strikethrough == rhs.strikethrough && lhs.horizontalAlign == rhs.horizontalAlign && lhs.verticalAlign == rhs.verticalAlign && lhs.locale == rhs.locale;
}

inline bool StyledRangesEqual(std::span<const StyledTextRange> lhs, std::span<const StyledTextRange> rhs) {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (size_t index = 0; index < lhs.size(); ++index) {
		if (lhs[index].start != rhs[index].start || lhs[index].length != rhs[index].length || !TextStyleEqual(lhs[index].style, rhs[index].style)) {
			return false;
		}
	}
	return true;
}

ComPtr<IDWriteTextLayout> CreateStyledTextLayout(
	IDWriteFactory* factory,
	IDWriteTextFormat* fallbackFormat,
	std::wstring_view text,
	float width,
	float height,
	const TextStyle* baseStyle,
	std::span<const StyledTextRange> ranges = {});

ComPtr<ID2D1SolidColorBrush> CreateBrush(ID2D1DeviceContext* context, const D2D1_COLOR_F& color);

class TextLayoutCache {
public:
	IDWriteTextLayout* GetOrCreate(
		IDWriteFactory* factory,
		IDWriteTextFormat* format,
		std::wstring_view text,
		float width,
		float height,
		const TextStyle* style = nullptr,
		std::span<const StyledTextRange> ranges = {},
		DWRITE_WORD_WRAPPING wrapping = DWRITE_WORD_WRAPPING_NO_WRAP) {
		const float effectiveWidth = (std::max)(1.0f, width);
		const float effectiveHeight = (std::max)(1.0f, height);
		const bool hasStyle = style != nullptr;
		if (!dirty_ && layout_ && factory_ == factory && format_ == format && text_ == text && NearlyEqual(width_, effectiveWidth) && NearlyEqual(height_, effectiveHeight) && wrapping_ == wrapping && hasStyle_ == hasStyle && (!hasStyle || TextStyleEqual(style_, *style)) && StyledRangesEqual(ranges_, ranges)) {
			return layout_.Get();
		}

		factory_ = factory;
		format_ = format;
		text_.assign(text.begin(), text.end());
		width_ = effectiveWidth;
		height_ = effectiveHeight;
		wrapping_ = wrapping;
		hasStyle_ = hasStyle;
		if (hasStyle) {
			style_ = *style;
		}
		ranges_.assign(ranges.begin(), ranges.end());
		layout_ = CreateStyledTextLayout(factory, format, text_, effectiveWidth, effectiveHeight, hasStyle ? &style_ : nullptr, ranges_);
		if (layout_) {
			layout_->SetWordWrapping(wrapping);
		}
		dirty_ = false;
		return layout_.Get();
	}

	void Invalidate() {
		dirty_ = true;
		layout_.Reset();
	}

private:
	ComPtr<IDWriteTextLayout> layout_;
	std::wstring text_;
	std::vector<StyledTextRange> ranges_;
	TextStyle style_{};
	IDWriteFactory* factory_ = nullptr;
	IDWriteTextFormat* format_ = nullptr;
	float width_ = 0.0f;
	float height_ = 0.0f;
	DWRITE_WORD_WRAPPING wrapping_ = DWRITE_WORD_WRAPPING_NO_WRAP;
	bool hasStyle_ = false;
	bool dirty_ = true;
};

struct ScrollbarState {
	ScrollOrientation orientation = ScrollOrientation::Vertical;
	D2D1_RECT_F viewport{ D2D1::RectF() };
	float contentExtent = 0.0f;
	float offset = 0.0f;
	float inset = 10.0f;
	float edgePadding = 2.0f;
	float baseThickness = 8.0f;
	float hoverThickness = 8.0f;
	float minThumbExtent = 24.0f;
	bool hovered = false;
	bool dragging = false;

	float ViewportExtent() const {
		return orientation == ScrollOrientation::Vertical ? (std::max)(1.0f, viewport.bottom - viewport.top) : (std::max)(1.0f, viewport.right - viewport.left);
	}

	float MaxOffset() const {
		return (std::max)(0.0f, contentExtent - ViewportExtent());
	}

	bool Visible() const {
		return MaxOffset() > 0.5f;
	}

	float Thickness() const {
		return dragging || hovered ? hoverThickness : baseThickness;
	}

	D2D1_RECT_F TrackBounds() const {
		const float thickness = Thickness();
		if (orientation == ScrollOrientation::Vertical) {
			const float right = viewport.right - edgePadding;
			return D2D1::RectF(right - thickness, viewport.top + inset, right, viewport.bottom - inset);
		}
		const float bottom = viewport.bottom - edgePadding;
		return D2D1::RectF(viewport.left + inset, bottom - thickness, viewport.right - inset, bottom);
	}

	D2D1_RECT_F ThumbBounds() const {
		const auto track = TrackBounds();
		const float trackExtent = orientation == ScrollOrientation::Vertical ? (std::max)(1.0f, track.bottom - track.top) : (std::max)(1.0f, track.right - track.left);
		const float viewportExtent = ViewportExtent();
		const float effectiveContent = (std::max)(contentExtent, viewportExtent);
		const float thumbExtent = (std::max)(minThumbExtent, trackExtent * (viewportExtent / effectiveContent));
		const float maxOffset = MaxOffset();
		const float normalized = maxOffset <= 0.0f ? 0.0f : (std::clamp)(offset / maxOffset, 0.0f, 1.0f);
		if (orientation == ScrollOrientation::Vertical) {
			const float thumbTop = track.top + (trackExtent - thumbExtent) * normalized;
			return D2D1::RectF(track.left, thumbTop, track.right, thumbTop + thumbExtent);
		}
		const float thumbLeft = track.left + (trackExtent - thumbExtent) * normalized;
		return D2D1::RectF(thumbLeft, track.top, thumbLeft + thumbExtent, track.bottom);
	}

	bool HitTrack(D2D1_POINT_2F point) const {
		return Visible() && UIComponent::PointInRect(TrackBounds(), point);
	}

	bool HitThumb(D2D1_POINT_2F point) const {
		return Visible() && UIComponent::PointInRect(ThumbBounds(), point);
	}
};

inline float ScrollbarOffsetForPointer(const ScrollbarState& state, float axisPoint) {
	const auto track = state.TrackBounds();
	const auto thumb = state.ThumbBounds();
	const float maxOffset = state.MaxOffset();
	if (maxOffset <= 0.0f) {
		return 0.0f;
	}
	const float trackStart = state.orientation == ScrollOrientation::Vertical ? track.top : track.left;
	const float trackEnd = state.orientation == ScrollOrientation::Vertical ? track.bottom : track.right;
	const float thumbExtent = state.orientation == ScrollOrientation::Vertical ? (thumb.bottom - thumb.top) : (thumb.right - thumb.left);
	const float usableExtent = (std::max)(1.0f, (trackEnd - trackStart) - thumbExtent);
	const float thumbStart = (std::clamp)(axisPoint - thumbExtent * 0.5f, trackStart, trackEnd - thumbExtent);
	return ((thumbStart - trackStart) / usableExtent) * maxOffset;
}

enum class SharedBrushSlot {
	Primary = 0,
	Secondary = 1,
	Tertiary = 2,
};

inline std::array<ComPtr<ID2D1SolidColorBrush>, 3>& SharedBrushPool() {
	static std::array<ComPtr<ID2D1SolidColorBrush>, 3> brushes;
	return brushes;
}

inline void EnsureSharedBrushPool(ID2D1DeviceContext* context) {
	if (!context) {
		return;
	}
	auto& brushes = SharedBrushPool();
	for (auto& brush : brushes) {
		if (!brush) {
			context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &brush);
		}
	}
}

inline D2D1_COLOR_F ApplyBrushOpacity(D2D1_COLOR_F color, float opacity) {
	color.a *= (std::clamp)(opacity, 0.0f, 1.0f);
	return color;
}

inline ID2D1SolidColorBrush* PrepareSharedBrush(ID2D1DeviceContext* context, SharedBrushSlot slot, const D2D1_COLOR_F& color, float opacity = 1.0f) {
	if (!context) {
		return nullptr;
	}
	EnsureSharedBrushPool(context);
	auto& brush = SharedBrushPool()[static_cast<size_t>(slot)];
	if (!brush) {
		return nullptr;
	}
	brush->SetOpacity(1.0f);
	brush->SetColor(ApplyBrushOpacity(color, opacity));
	return brush.Get();
}

inline void DrawRadixScrollbar(ID2D1DeviceContext* context, const ScrollbarState& state, ID2D1SolidColorBrush* thumbSource, ID2D1SolidColorBrush* trackSource, float visibility = 1.0f) {
	if (!context || !state.Visible() || !thumbSource || !trackSource || visibility <= 0.001f) {
		return;
	}
	const auto track = state.TrackBounds();
	auto thumb = state.ThumbBounds();
	const auto trackColor = trackSource->GetColor();
	const auto thumbColor = thumbSource->GetColor();
	const float trackOpacity = (state.dragging ? 0.22f : (state.hovered ? 0.18f : 0.14f)) * visibility;
	const float thumbOpacity = (state.dragging ? 0.84f : (state.hovered ? 0.74f : 0.62f)) * visibility;
	if (state.orientation == ScrollOrientation::Vertical) {
		thumb.left += 0.5f;
		thumb.right -= 0.5f;
	}
	else {
		thumb.top += 0.5f;
		thumb.bottom -= 0.5f;
	}
	const auto roundedRect = [](const D2D1_RECT_F& rect) {
		const float width = (std::max)(0.0f, rect.right - rect.left);
		const float height = (std::max)(0.0f, rect.bottom - rect.top);
		const float radius = (std::max)(1.0f, (std::min)(width, height) * 0.5f);
		return D2D1::RoundedRect(rect, radius, radius);
	};
	auto* trackBrush = PrepareSharedBrush(context, SharedBrushSlot::Primary, trackColor, trackOpacity);
	auto* thumbBrush = PrepareSharedBrush(context, SharedBrushSlot::Secondary, thumbColor, thumbOpacity);
	if (!trackBrush || !thumbBrush) {
		return;
	}
	context->FillRoundedRectangle(roundedRect(track), trackBrush);
	context->FillRoundedRectangle(roundedRect(thumb), thumbBrush);
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

inline TextStyle CaptureTextStyle(IDWriteTextFormat* format, ID2D1SolidColorBrush* brush) {
	TextStyle style;
	if (format) {
		UINT32 length = format->GetFontFamilyNameLength();
		style.fontFamily.resize(length + 1);
		if (length > 0) {
			format->GetFontFamilyName(style.fontFamily.data(), length + 1);
			style.fontFamily.resize(length);
		}
		else {
			style.fontFamily = L"Segoe UI";
		}
		style.fontSize = format->GetFontSize();
		style.weight = format->GetFontWeight();
		style.style = format->GetFontStyle();
		style.stretch = format->GetFontStretch();
		UINT32 localeLength = format->GetLocaleNameLength();
		style.locale.resize(localeLength + 1);
		if (localeLength > 0) {
			format->GetLocaleName(style.locale.data(), localeLength + 1);
			style.locale.resize(localeLength);
		}
		else {
			style.locale = L"en-us";
		}
	}
	if (brush) {
		style.color = brush->GetColor();
	}
	return style;
}

inline TextStyle CaptureTextStyle(IDWriteTextFormat* format, const D2D1_COLOR_F& color) {
	TextStyle style = CaptureTextStyle(format, static_cast<ID2D1SolidColorBrush*>(nullptr));
	style.color = color;
	return style;
}

inline ComPtr<IDWriteTextFormat> CreateTextFormat(IDWriteFactory* factory, const TextStyle& style) {
	if (!factory) {
		return nullptr;
	}
	ComPtr<IDWriteTextFormat> format;
	if (FAILED(factory->CreateTextFormat(style.fontFamily.c_str(), nullptr, style.weight, style.style, style.stretch, style.fontSize, style.locale.c_str(), &format))) {
		return nullptr;
	}
	switch (style.horizontalAlign) {
	case HorizontalAlign::Left:
		format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
		break;
	case HorizontalAlign::Right:
		format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
		break;
	default:
		format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		break;
	}
	switch (style.verticalAlign) {
	case VerticalAlign::Top:
		format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
		break;
	case VerticalAlign::Bottom:
		format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
		break;
	default:
		format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		break;
	}
	return format;
}

inline void ApplyTextStyleToLayout(IDWriteTextLayout* layout, const TextStyle& style, const DWRITE_TEXT_RANGE& range) {
	if (!layout) {
		return;
	}
	layout->SetFontFamilyName(style.fontFamily.c_str(), range);
	layout->SetFontSize(style.fontSize, range);
	layout->SetFontWeight(style.weight, range);
	layout->SetFontStyle(style.style, range);
	layout->SetFontStretch(style.stretch, range);
	layout->SetUnderline(style.underline, range);
	layout->SetStrikethrough(style.strikethrough, range);
	if (range.startPosition == 0) {
		switch (style.horizontalAlign) {
		case HorizontalAlign::Left:
			layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
			break;
		case HorizontalAlign::Right:
			layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
			break;
		default:
			layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
			break;
		}
		switch (style.verticalAlign) {
		case VerticalAlign::Top:
			layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
			break;
		case VerticalAlign::Bottom:
			layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
			break;
		default:
			layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
			break;
		}
	}
}

inline ComPtr<IDWriteTextLayout> CreateStyledTextLayout(
	IDWriteFactory* factory,
	IDWriteTextFormat* fallbackFormat,
	std::wstring_view text,
	float width,
	float height,
	const TextStyle* baseStyle,
	std::span<const StyledTextRange> ranges) {
	auto effectiveFormat = baseStyle ? CreateTextFormat(factory, *baseStyle) : nullptr;
	auto layout = CreateTextLayout(factory, effectiveFormat ? effectiveFormat.Get() : fallbackFormat, text, width, height);
	if (!layout) {
		return nullptr;
	}
	if (baseStyle) {
		ApplyTextStyleToLayout(layout.Get(), *baseStyle, DWRITE_TEXT_RANGE{ 0, static_cast<UINT32>(text.size()) });
	}
	for (const auto& range : ranges) {
		if (range.length == 0 || range.start >= text.size()) {
			continue;
		}
		const UINT32 start = static_cast<UINT32>(range.start);
		const UINT32 length = static_cast<UINT32>((std::min)(range.length, text.size() - range.start));
		ApplyTextStyleToLayout(layout.Get(), range.style, DWRITE_TEXT_RANGE{ start, length });
	}
	return layout;
}

inline DWRITE_TEXT_METRICS MeasureTextMetrics(
	IDWriteFactory* factory,
	IDWriteTextFormat* fallbackFormat,
	std::wstring_view text,
	const TextStyle* style = nullptr,
	DWRITE_WORD_WRAPPING wrapping = DWRITE_WORD_WRAPPING_NO_WRAP) {
	DWRITE_TEXT_METRICS metrics{};
	auto layout = CreateStyledTextLayout(factory, fallbackFormat, text, 4096.0f, 4096.0f, style);
	if (!layout) {
		return metrics;
	}
	layout->SetWordWrapping(wrapping);
	layout->GetMetrics(&metrics);
	return metrics;
}

inline ComPtr<ID2D1SolidColorBrush> CreateBrush(ID2D1DeviceContext* context, const D2D1_COLOR_F& color) {
	if (!context) {
		return nullptr;
	}
	ComPtr<ID2D1SolidColorBrush> brush;
	context->CreateSolidColorBrush(color, &brush);
	return brush;
}

inline void DrawStyledText(
	ID2D1DeviceContext* context,
	IDWriteFactory* factory,
	IDWriteTextFormat* fallbackFormat,
	ID2D1SolidColorBrush* fallbackBrush,
	std::wstring_view text,
	const D2D1_RECT_F& rect,
	const TextStyle* style = nullptr,
	std::span<const StyledTextRange> ranges = {},
	DWRITE_WORD_WRAPPING wrapping = DWRITE_WORD_WRAPPING_NO_WRAP) {
	if (!context || !fallbackFormat || !fallbackBrush || rect.right <= rect.left || rect.bottom <= rect.top) {
		return;
	}
	if (factory) {
		auto layout = CreateStyledTextLayout(factory, fallbackFormat, text, rect.right - rect.left, rect.bottom - rect.top, style, ranges);
		if (layout) {
			layout->SetWordWrapping(wrapping);
			ID2D1SolidColorBrush* drawBrush = fallbackBrush;
			if (style) {
				auto* overrideBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, style->color);
				drawBrush = overrideBrush ? overrideBrush : fallbackBrush;
			}
			context->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), drawBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
			return;
		}
	}
	context->DrawTextW(text.data(), static_cast<UINT32>(text.size()), fallbackFormat, rect, fallbackBrush);
}

inline DWRITE_TEXT_ALIGNMENT ToDWrite(HorizontalAlign align) {
	switch (align) {
	case HorizontalAlign::Left:
		return DWRITE_TEXT_ALIGNMENT_LEADING;
	case HorizontalAlign::Right:
		return DWRITE_TEXT_ALIGNMENT_TRAILING;
	default:
		return DWRITE_TEXT_ALIGNMENT_CENTER;
	}
}

inline DWRITE_PARAGRAPH_ALIGNMENT ToDWrite(VerticalAlign align) {
	switch (align) {
	case VerticalAlign::Top:
		return DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
	case VerticalAlign::Bottom:
		return DWRITE_PARAGRAPH_ALIGNMENT_FAR;
	default:
		return DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
	}
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
	TextBlock(std::wstring text, ComPtr<IDWriteFactory> dwriteFactory, ComPtr<IDWriteTextFormat> format, const D2D1_COLOR_F& color, bool dynamicText = false)
		: text_(std::move(text)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), color_(color), dynamicText_(dynamicText), textStyle_(CaptureTextStyle(format_.Get(), color_)) {}

	void SetText(std::wstring text) {
		text_ = std::move(text);
	}

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		styleOverride_ = true;
	}

	void SetStyledRanges(std::vector<StyledTextRange> ranges) {
		styledRanges_ = std::move(ranges);
	}

	bool IsDynamic() const override {
		return dynamicText_;
	}

	void Render(ID2D1DeviceContext* context) override {
		if (!visible_ || !format_) {
			return;
		}
		const TextStyle* baseStyle = styleOverride_ ? &textStyle_ : nullptr;
		auto* layout = layoutCache_.GetOrCreate(dwriteFactory_.Get(), format_.Get(), text_, bounds_.right - bounds_.left, bounds_.bottom - bounds_.top, baseStyle, styledRanges_);
		auto* brush = PrepareSharedBrush(context, SharedBrushSlot::Primary, styleOverride_ ? textStyle_.color : color_);
		if (layout) {
			context->DrawTextLayout(D2D1::Point2F(bounds_.left, bounds_.top), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
		}
	}

private:
	std::wstring text_;
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F color_{};
	bool dynamicText_ = false;
	TextStyle textStyle_{};
	bool styleOverride_ = false;
	std::vector<StyledTextRange> styledRanges_;
	mutable TextLayoutCache layoutCache_;
};

class ExpandableNote final : public UIComponent {
public:
	ExpandableNote(std::wstring title, std::wstring body, ComPtr<IDWriteFactory> dwriteFactory, ComPtr<IDWriteTextFormat> titleFormat, ComPtr<IDWriteTextFormat> bodyFormat, const D2D1_COLOR_F& surfaceColor, const D2D1_COLOR_F& outlineColor, const D2D1_COLOR_F& textColor, const D2D1_COLOR_F& mutedColor)
		: title_(std::move(title)), body_(std::move(body)), dwriteFactory_(std::move(dwriteFactory)), titleFormat_(std::move(titleFormat)), bodyFormat_(std::move(bodyFormat)), surfaceColor_(surfaceColor), outlineColor_(outlineColor), textColor_(textColor), mutedColor_(mutedColor), titleStyle_(CaptureTextStyle(titleFormat_.Get(), textColor_)), bodyStyle_(CaptureTextStyle(bodyFormat_.Get(), mutedColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }
	CursorKind Cursor() const override { return CursorKind::Hand; }

	void SetTitle(std::wstring title) {
		title_ = std::move(title);
	}

	void SetBody(std::wstring body) {
		body_ = std::move(body);
	}

	void SetTitleStyle(const TextStyle& style) {
		titleStyle_ = style;
		titleStyleOverride_ = true;
	}

	void SetBodyStyle(const TextStyle& style) {
		bodyStyle_ = style;
		bodyStyleOverride_ = true;
	}

	void OnAttachAnimations() override {
		RegisterAnimationSlot(L"expand", expanded_ ? 1.0f : 0.0f);
	}

	void Render(ID2D1DeviceContext* context) override {
		auto* surfaceBrush = PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_);
		auto* outlineBrush = PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.4f + 0.5f * FocusProgress());
		auto* textBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, titleStyleOverride_ ? titleStyle_.color : textColor_);
		context->FillRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), surfaceBrush);
		context->DrawRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), outlineBrush, 1.0f + FocusProgress());
		const D2D1_RECT_F headerRect = D2D1::RectF(bounds_.left + 14.0f, bounds_.top + 8.0f, bounds_.right - 28.0f, bounds_.top + 30.0f);
		DrawStyledText(context, dwriteFactory_.Get(), titleFormat_.Get(), textBrush, title_, headerRect, titleStyleOverride_ ? &titleStyle_ : nullptr);
		const float expand = AnimationSlotValue(L"expand", 0.0f);
		const float arrowCenterX = bounds_.right - 16.0f;
		const float arrowCenterY = bounds_.top + 18.0f;
		const float arrowLift = 3.0f * expand;
		context->DrawLine(D2D1::Point2F(arrowCenterX - 5.0f, arrowCenterY - 2.0f + arrowLift), D2D1::Point2F(arrowCenterX, arrowCenterY + 3.0f - arrowLift), outlineBrush, 1.5f);
		context->DrawLine(D2D1::Point2F(arrowCenterX, arrowCenterY + 3.0f - arrowLift), D2D1::Point2F(arrowCenterX + 5.0f, arrowCenterY - 2.0f + arrowLift), outlineBrush, 1.5f);
		if (expand <= 0.01f) {
			return;
		}
		const D2D1_RECT_F bodyRect = D2D1::RectF(bounds_.left + 14.0f, bounds_.top + 34.0f, bounds_.right - 14.0f, bounds_.bottom - 12.0f);
		const float visibleHeight = (bodyRect.bottom - bodyRect.top) * expand;
		const D2D1_RECT_F clip = D2D1::RectF(bodyRect.left, bodyRect.top, bodyRect.right, bodyRect.top + visibleHeight);
		context->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		if (bodyStyleOverride_) {
			TextStyle animatedBodyStyle = bodyStyle_;
			animatedBodyStyle.color.a *= expand;
			auto* mutedBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, bodyStyle_.color, expand);
			DrawStyledText(context, dwriteFactory_.Get(), bodyFormat_.Get(), mutedBrush, body_, bodyRect, &animatedBodyStyle, {}, DWRITE_WORD_WRAPPING_WRAP);
		}
		else {
			auto* mutedBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, mutedColor_, expand);
			DrawStyledText(context, dwriteFactory_.Get(), bodyFormat_.Get(), mutedBrush, body_, bodyRect, nullptr, {}, DWRITE_WORD_WRAPPING_WRAP);
		}
		context->PopAxisAlignedClip();
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); }

	void OnPointerUp(D2D1_POINT_2F point) override {
		const bool shouldToggle = pressed_ && PointInRect(bounds_, point);
		UIComponent::OnPointerUp(point);
		if (shouldToggle) {
			expanded_ = !expanded_;
			AnimateSlot(L"expand", expanded_ ? 1.0f : 0.0f, 0.18, 0.25, 0.25);
		}
	}

	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_SPACE || key == VK_RETURN) {
			expanded_ = !expanded_;
			AnimateSlot(L"expand", expanded_ ? 1.0f : 0.0f, 0.18, 0.25, 0.25);
		}
	}

private:
	std::wstring title_;
	std::wstring body_;
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> titleFormat_;
	ComPtr<IDWriteTextFormat> bodyFormat_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F textColor_{};
	D2D1_COLOR_F mutedColor_{};
	TextStyle titleStyle_{};
	TextStyle bodyStyle_{};
	bool titleStyleOverride_ = false;
	bool bodyStyleOverride_ = false;
	bool expanded_ = true;
};

class ScrollArea final : public UIComponent {
public:
	ScrollArea(const D2D1_COLOR_F& fillColor, const D2D1_COLOR_F& outlineColor, const D2D1_COLOR_F& scrollThumbColor, const D2D1_COLOR_F& scrollTrackColor)
		: fillColor_(fillColor), outlineColor_(outlineColor), scrollThumbColor_(scrollThumbColor), scrollTrackColor_(scrollTrackColor) {}

	bool IsDynamic() const override {
		return scrollBar_.dragging || ScrollBarOpacity() > 0.01f || ScrollRevealActive();
	}

	bool WantsFrameTick() const override {
		return ScrollRevealActive();
	}

	void OnAttachAnimations() override {
		RegisterAnimationSlot(L"scrollbarVisibility", 0.0f);
	}

	void SetContentMetrics(float contentHeight) {
		const float previousContentHeight = contentHeight_;
		const bool hadOverflow = ActualNeedsVerticalScrollBar(previousContentHeight);
		contentHeight_ = (std::max)(contentHeight, 0.0f);
		const bool hasOverflow = ActualNeedsVerticalScrollBar(contentHeight_);
		if (hadOverflow != hasOverflow) {
			hintedContentHeight_ = hasOverflow ? contentHeight_ : previousContentHeight;
			TouchScrollReveal();
		}
		UpdateScrollBarViewport();
		SetScrollOffset(scrollOffset_);
		UpdateScrollBarAnimation(ShouldRevealScrollBar());
	}

	float ScrollOffset() const {
		return scrollOffset_;
	}

	void SetScrollOffset(float offset) {
		UpdateScrollBarViewport();
		scrollOffset_ = (std::clamp)(offset, 0.0f, MaxScrollOffset());
		scrollBar_.offset = scrollOffset_;
	}

	float MaxScrollOffset() const {
		UpdateScrollBarViewport();
		return (std::max)(0.0f, contentHeight_ - ContentViewportHeight());
	}

	bool NeedsVerticalScrollBar() const {
		return ActualNeedsVerticalScrollBar(contentHeight_);
	}

	D2D1_RECT_F ContentClipBounds() const {
		return LayoutBounds(ShouldReserveVerticalScrollBar());
	}

	bool ShouldReserveVerticalScrollBar() const {
		UpdateScrollBarViewport();
		return scrollBar_.Visible() || ScrollBarOpacity() > 0.01f;
	}

	D2D1_RECT_F LayoutBounds(bool reserveScrollbar) const {
		auto clip = BaseContentClipBounds();
		if (reserveScrollbar) {
			clip.right = (std::max)(clip.left + 1.0f, clip.right - kScrollbarGutter);
		}
		return clip;
	}

	bool HitScrollBar(D2D1_POINT_2F point) const {
		UpdateScrollBarViewport();
		return scrollBar_.HitTrack(point);
	}

	bool ContainsContent(D2D1_POINT_2F point) const {
		return PointInRect(ContentClipBounds(), point);
	}

	void Render(ID2D1DeviceContext* context) override {
		if (!visible_) {
			return;
		}
		UpdateScrollBarAnimation(ShouldRevealScrollBar());
		auto rounded = D2D1::RoundedRect(bounds_, kCardRadius, kCardRadius);
		context->FillRoundedRectangle(rounded, PrepareSharedBrush(context, SharedBrushSlot::Primary, fillColor_));
		context->DrawRoundedRectangle(rounded, PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_), 1.0f);
		DrawRadixScrollbar(context, scrollBar_, PrepareSharedBrush(context, SharedBrushSlot::Primary, scrollThumbColor_), PrepareSharedBrush(context, SharedBrushSlot::Secondary, scrollTrackColor_), ScrollBarOpacity());
	}

	CursorKind CursorAt(D2D1_POINT_2F point) const override {
		scrollBar_.hovered = scrollBar_.Visible() && HitScrollBar(point);
		if (scrollBar_.hovered) {
			TouchScrollReveal();
		}
		return HitScrollBar(point) ? CursorKind::Arrow : CursorKind::Arrow;
	}

	void OnHover(bool hovered) override {
		UIComponent::OnHover(hovered);
		if (!hovered && !scrollBar_.dragging) {
			scrollBar_.hovered = false;
		}
	}

	void OnPointerDown(D2D1_POINT_2F point) override {
		if (!HitScrollBar(point)) {
			return;
		}
		UIComponent::OnPointerDown(point);
		scrollBar_.dragging = true;
		scrollBar_.hovered = true;
		TouchScrollReveal();
		dragStartY_ = point.y;
		startScrollOffset_ = scrollOffset_;
		if (!scrollBar_.HitThumb(point)) {
			SetScrollOffset(ScrollbarOffsetForPointer(scrollBar_, point.y));
			startScrollOffset_ = scrollOffset_;
			dragStartY_ = point.y;
		}
	}

	void OnPointerMove(D2D1_POINT_2F point) override {
		scrollBar_.hovered = scrollBar_.HitTrack(point);
		if (!scrollBar_.dragging) {
			return;
		}
		TouchScrollReveal();
		const auto track = scrollBar_.TrackBounds();
		const auto thumb = scrollBar_.ThumbBounds();
		const float travel = (std::max)(1.0f, (track.bottom - track.top) - (thumb.bottom - thumb.top));
		const float maxOffset = MaxScrollOffset();
		if (maxOffset <= 0.0f) {
			return;
		}
		const float ratio = (point.y - dragStartY_) / travel;
		SetScrollOffset(startScrollOffset_ + ratio * maxOffset);
	}

	void OnPointerUp(D2D1_POINT_2F point) override {
		if (scrollBar_.dragging) {
			OnPointerMove(point);
		}
		scrollBar_.dragging = false;
		scrollBar_.hovered = scrollBar_.HitTrack(point);
		if (scrollBar_.hovered) {
			TouchScrollReveal();
		}
		UIComponent::OnPointerUp(point);
	}

	bool OnMouseWheel(int delta, D2D1_POINT_2F point) override {
		if (!NeedsVerticalScrollBar() || (!ContainsContent(point) && !HitScrollBar(point))) {
			return false;
		}
		const float previousOffset = scrollOffset_;
		SetScrollOffset(scrollOffset_ + (delta > 0 ? -36.0f : 36.0f));
		const bool changed = !NearlyEqual(previousOffset, scrollOffset_, 0.05f);
		if (changed) {
			TouchScrollReveal();
		}
		return changed;
	}

protected:
	HRESULT CreateHitGeometry(ID2D1Factory1* factory, ID2D1Geometry** geometry) const override {
		if (!factory || !geometry) {
			return E_INVALIDARG;
		}
		return factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(bounds_, kCardRadius, kCardRadius), reinterpret_cast<ID2D1RoundedRectangleGeometry**>(geometry));
	}

private:
	float ScrollBarOpacity() const {
		return AnimationSlotValue(L"scrollbarVisibility", 0.0f);
	}

	bool ScrollRevealActive() const {
		return scrollRevealUntil_ > std::chrono::steady_clock::now();
	}

	bool OverflowHintActive() const {
		return hintedContentHeight_ > 0.0f && ScrollRevealActive();
	}

	bool ActualNeedsVerticalScrollBar(float contentHeight) const {
		return contentHeight > ContentViewportHeight() + 0.5f;
	}

	bool ShouldRevealScrollBar() const {
		UpdateScrollBarViewport();
		return scrollBar_.Visible() && (scrollBar_.dragging || scrollBar_.hovered || ScrollRevealActive());
	}

	void TouchScrollReveal() const {
		scrollRevealUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
	}

	void UpdateScrollBarAnimation(bool visible) {
		if (visible == scrollBarVisibleTarget_) {
			return;
		}
		scrollBarVisibleTarget_ = visible;
		AnimateSlotLinear(L"scrollbarVisibility", visible ? 1.0f : 0.0f, 0.16);
	}

	void UpdateScrollBarViewport() const {
		scrollBar_.viewport = BaseContentClipBounds();
		scrollBar_.contentExtent = OverflowHintActive() ? (std::max)(contentHeight_, hintedContentHeight_) : contentHeight_;
		scrollBar_.offset = scrollOffset_;
	}

	float ContentViewportHeight() const {
		UpdateScrollBarViewport();
		const auto clip = BaseContentClipBounds();
		return (std::max)(1.0f, clip.bottom - clip.top);
	}

	D2D1_RECT_F BaseContentClipBounds() const {
		return D2D1::RectF(bounds_.left + 2.0f, bounds_.top + 2.0f, bounds_.right - 2.0f, bounds_.bottom - 2.0f);
	}

	D2D1_COLOR_F fillColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F scrollThumbColor_{};
	D2D1_COLOR_F scrollTrackColor_{};
	float contentHeight_ = 0.0f;
	float scrollOffset_ = 0.0f;
	float dragStartY_ = 0.0f;
	float startScrollOffset_ = 0.0f;
	bool scrollBarVisibleTarget_ = false;
	float hintedContentHeight_ = 0.0f;
	mutable std::chrono::steady_clock::time_point scrollRevealUntil_{};
	mutable ScrollbarState scrollBar_{ ScrollOrientation::Vertical };
	static constexpr float kScrollbarGutter = 18.0f;
};

class Button final : public UIComponent {
public:
	Button(std::wstring text,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& primaryFillColor,
		const D2D1_COLOR_F& secondaryFillColor,
		const D2D1_COLOR_F& outlineColor,
		const D2D1_COLOR_F& textColor,
		std::function<void()> onClick,
		bool primary)
		: text_(std::move(text)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), primaryFillColor_(primaryFillColor), secondaryFillColor_(secondaryFillColor), outlineColor_(outlineColor), textColor_(textColor), onClick_(std::move(onClick)), primary_(primary), textStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }
	CursorKind Cursor() const override { return CursorKind::Hand; }

	void SetTextAlignment(HorizontalAlign horizontal, VerticalAlign vertical) {
		horizontalAlign_ = horizontal;
		verticalAlign_ = vertical;
	}

	void SetTextPadding(float leftRight, float topBottom) {
		textPaddingX_ = leftRight;
		textPaddingY_ = topBottom;
	}

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		textStyleOverride_ = true;
	}

	void OnAttachAnimations() override {
		RegisterAnimationSlot(L"scrollbarVisibility", 0.0f);
	}

	void Render(ID2D1DeviceContext* context) override {
		auto rounded = D2D1::RoundedRect(bounds_, 12.0f, 12.0f);
		auto* fill = PrepareSharedBrush(context, SharedBrushSlot::Primary, primary_ ? primaryFillColor_ : secondaryFillColor_, 1.0f - 0.18f * HoverProgress() - 0.12f * PressProgress());
		const float hover = HoverProgress();
		const float press = PressProgress();
		context->FillRoundedRectangle(rounded, fill);
		auto* outlineBrush = PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.55f + 0.45f * FocusProgress());
		context->DrawRoundedRectangle(rounded, outlineBrush, 1.0f + FocusProgress());
		const D2D1_RECT_F textRect = D2D1::RectF(bounds_.left + textPaddingX_, bounds_.top + textPaddingY_, bounds_.right - textPaddingX_, bounds_.bottom - textPaddingY_);
		TextStyle effectiveStyle = textStyle_;
		effectiveStyle.horizontalAlign = horizontalAlign_;
		effectiveStyle.verticalAlign = verticalAlign_;
		effectiveStyle.color = textStyleOverride_ ? textStyle_.color : ApplyBrushOpacity(textColor_, enabled_ ? 1.0f : 0.5f);
		auto* layout = layoutCache_.GetOrCreate(dwriteFactory_.Get(), format_.Get(), text_, textRect.right - textRect.left, textRect.bottom - textRect.top, &effectiveStyle);
		auto* styledBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, effectiveStyle.color);
		if (layout) {
			context->DrawTextLayout(D2D1::Point2F(textRect.left, textRect.top), layout, styledBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
		}
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
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F primaryFillColor_{};
	D2D1_COLOR_F secondaryFillColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F textColor_{};
	std::function<void()> onClick_;
	bool primary_ = false;
	HorizontalAlign horizontalAlign_ = HorizontalAlign::Center;
	VerticalAlign verticalAlign_ = VerticalAlign::Center;
	float textPaddingX_ = 14.0f;
	float textPaddingY_ = 6.0f;
	TextStyle textStyle_{};
	bool textStyleOverride_ = false;
	mutable TextLayoutCache layoutCache_;
};

class ImageFrame final : public UIComponent {
public:
	ImageFrame(const D2D1_COLOR_F& outlineColor, const D2D1_COLOR_F& accentColor, const D2D1_COLOR_F& mutedColor)
		: outlineColor_(outlineColor), accentColor_(accentColor), mutedColor_(mutedColor) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }
	CursorKind Cursor() const override { return CursorKind::Arrow; }
	bool WantsFrameTick() const override {
		return HorizontalScrollRevealActive() || VerticalScrollRevealActive();
	}

	void OnAttachAnimations() override {
		RegisterAnimationSlot(L"horizontalScrollbarVisibility", 0.0f);
		RegisterAnimationSlot(L"verticalScrollbarVisibility", 0.0f);
	}

	void SetScrollOffset(float x, float y) {
		scrollX_ = Clamp01(x);
		scrollY_ = Clamp01(y);
	}

	void Render(ID2D1DeviceContext* context) override {
		const auto metrics = ComputeMetrics();
		UpdateScrollBarAnimations(metrics);
		auto rounded = D2D1::RoundedRect(bounds_, 14.0f, 14.0f);
		context->FillRoundedRectangle(rounded, PrepareSharedBrush(context, SharedBrushSlot::Primary, mutedColor_));
		const float offsetX = (metrics.contentWidth - metrics.viewportWidth) * scrollX_;
		const float offsetY = (metrics.contentHeight - metrics.viewportHeight) * scrollY_;
		context->PushAxisAlignedClip(metrics.viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		for (int row = 0; row < 5; ++row) {
			for (int col = 0; col < 8; ++col) {
				const float tileW = metrics.contentWidth / 8.0f;
				const float tileH = metrics.contentHeight / 5.0f;
				const float left = metrics.viewport.left + tileW * col - offsetX;
				const float top = metrics.viewport.top + tileH * row - offsetY;
				const float alpha = ((row + col) % 2 == 0) ? 0.18f : 0.08f;
				context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, left + tileW - 8.0f, top + tileH - 8.0f), 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_, alpha));
			}
		}
		auto* accentBrush = PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_, 0.78f);
		context->DrawLine(D2D1::Point2F(metrics.viewport.left + 16.0f - offsetX, metrics.viewport.top + metrics.contentHeight * 0.72f - offsetY), D2D1::Point2F(metrics.viewport.left + metrics.contentWidth * 0.45f - offsetX, metrics.viewport.top + metrics.contentHeight * 0.34f - offsetY), accentBrush, 2.0f);
		context->DrawLine(D2D1::Point2F(metrics.viewport.left + metrics.contentWidth * 0.45f - offsetX, metrics.viewport.top + metrics.contentHeight * 0.34f - offsetY), D2D1::Point2F(metrics.viewport.left + metrics.contentWidth - 16.0f - offsetX, metrics.viewport.top + metrics.contentHeight * 0.64f - offsetY), accentBrush, 2.0f);
		context->PopAxisAlignedClip();
		DrawScrollBars(context, metrics);
		context->DrawRoundedRectangle(rounded, PrepareSharedBrush(context, SharedBrushSlot::Tertiary, outlineColor_, 0.55f), 1.0f);
	}

	void OnPointerDown(D2D1_POINT_2F point) override {
		UIComponent::OnPointerDown(point);
		const auto metrics = ComputeMetrics();
		dragMode_ = DragMode::None;
		if (metrics.showHorizontal && metrics.horizontalScroll.HitThumb(point)) {
			dragMode_ = DragMode::Horizontal;
			TouchHorizontalScrollReveal();
		}
		else if (metrics.showVertical && metrics.verticalScroll.HitThumb(point)) {
			dragMode_ = DragMode::Vertical;
			TouchVerticalScrollReveal();
		}
		if (dragMode_ != DragMode::None) {
			dragOrigin_ = point;
			originScrollX_ = scrollX_;
			originScrollY_ = scrollY_;
		}
	}

	void OnPointerMove(D2D1_POINT_2F point) override {
		if (!pressed_ || dragMode_ == DragMode::None) {
			return;
		}
		const auto metrics = ComputeMetrics();
		if (dragMode_ == DragMode::Horizontal) {
			TouchHorizontalScrollReveal();
			const auto track = metrics.horizontalScroll.TrackBounds();
			const auto thumb = metrics.horizontalScroll.ThumbBounds();
			const float trackWidth = (std::max)(1.0f, (track.right - track.left) - (thumb.right - thumb.left));
			scrollX_ = Clamp01(originScrollX_ + (point.x - dragOrigin_.x) / trackWidth);
			return;
		}
		TouchVerticalScrollReveal();
		const auto track = metrics.verticalScroll.TrackBounds();
		const auto thumb = metrics.verticalScroll.ThumbBounds();
		const float trackHeight = (std::max)(1.0f, (track.bottom - track.top) - (thumb.bottom - thumb.top));
		scrollY_ = Clamp01(originScrollY_ + (point.y - dragOrigin_.y) / trackHeight);
	}

	void OnPointerUp(D2D1_POINT_2F point) override {
		dragMode_ = DragMode::None;
		UIComponent::OnPointerUp(point);
	}

	void OnHover(bool hovered) override {
		UIComponent::OnHover(hovered);
		if (!hovered && dragMode_ == DragMode::None) {
			horizontalScrollHovered_ = false;
			verticalScrollHovered_ = false;
		}
	}

	bool OnMouseWheel(int delta, D2D1_POINT_2F point) override {
		if (!PointInRect(bounds_, point)) {
			return false;
		}
		const float previous = scrollY_;
		scrollY_ = Clamp01(scrollY_ + (delta > 0 ? -0.05f : 0.05f));
		const bool changed = !NearlyEqual(previous, scrollY_, 0.0001f);
		if (changed) {
			TouchVerticalScrollReveal();
		}
		return changed;
	}

	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_LEFT) scrollX_ = Clamp01(scrollX_ - 0.04f);
		if (key == VK_RIGHT) scrollX_ = Clamp01(scrollX_ + 0.04f);
		if (key == VK_UP) scrollY_ = Clamp01(scrollY_ - 0.04f);
		if (key == VK_DOWN) scrollY_ = Clamp01(scrollY_ + 0.04f);
	}

private:
	struct Metrics {
		D2D1_RECT_F viewport{ D2D1::RectF() };
		float viewportWidth = 0.0f;
		float viewportHeight = 0.0f;
		float contentWidth = 0.0f;
		float contentHeight = 0.0f;
		bool showHorizontal = false;
		bool showVertical = false;
		bool renderHorizontal = false;
		bool renderVertical = false;
		float horizontalOpacity = 0.0f;
		float verticalOpacity = 0.0f;
		ScrollbarState horizontalScroll{ ScrollOrientation::Horizontal };
		ScrollbarState verticalScroll{ ScrollOrientation::Vertical };
	};

	enum class DragMode {
		None,
		Horizontal,
		Vertical,
	};

	Metrics ComputeMetrics() const {
		Metrics metrics;
		const D2D1_RECT_F baseViewport = InflateRect(bounds_, 12.0f);
		metrics.viewport = baseViewport;
		const float rawWidth = (std::max)(1.0f, baseViewport.right - baseViewport.left);
		const float rawHeight = (std::max)(1.0f, baseViewport.bottom - baseViewport.top);
		metrics.contentWidth = rawWidth * 1.65f;
		metrics.contentHeight = rawHeight * 1.75f;
		metrics.showHorizontal = metrics.contentWidth > rawWidth + 1.0f;
		metrics.showVertical = metrics.contentHeight > rawHeight + 1.0f;
		metrics.horizontalOpacity = HorizontalScrollOpacity();
		metrics.verticalOpacity = VerticalScrollOpacity();
		metrics.renderHorizontal = metrics.showHorizontal || metrics.horizontalOpacity > 0.01f;
		metrics.renderVertical = metrics.showVertical || metrics.verticalOpacity > 0.01f;
		if (metrics.renderVertical) {
			metrics.viewport.right -= kScrollGutter;
		}
		if (metrics.renderHorizontal) {
			metrics.viewport.bottom -= kScrollGutter;
		}
		metrics.viewportWidth = (std::max)(1.0f, metrics.viewport.right - metrics.viewport.left);
		metrics.viewportHeight = (std::max)(1.0f, metrics.viewport.bottom - metrics.viewport.top);
		if (metrics.renderHorizontal) {
			metrics.horizontalScroll.viewport = baseViewport;
			metrics.horizontalScroll.contentExtent = metrics.contentWidth;
			metrics.horizontalScroll.offset = scrollX_ * (std::max)(0.0f, metrics.contentWidth - metrics.viewportWidth);
			metrics.horizontalScroll.hovered = horizontalScrollHovered_ || dragMode_ == DragMode::Horizontal;
			metrics.horizontalScroll.dragging = dragMode_ == DragMode::Horizontal;
		}
		if (metrics.renderVertical) {
			metrics.verticalScroll.viewport = baseViewport;
			metrics.verticalScroll.contentExtent = metrics.contentHeight;
			metrics.verticalScroll.offset = scrollY_ * (std::max)(0.0f, metrics.contentHeight - metrics.viewportHeight);
			metrics.verticalScroll.hovered = verticalScrollHovered_ || dragMode_ == DragMode::Vertical;
			metrics.verticalScroll.dragging = dragMode_ == DragMode::Vertical;
		}
		return metrics;
	}

	CursorKind CursorAt(D2D1_POINT_2F point) const override {
		const auto metrics = ComputeMetrics();
		horizontalScrollHovered_ = metrics.showHorizontal && metrics.horizontalScroll.HitTrack(point);
		verticalScrollHovered_ = metrics.showVertical && metrics.verticalScroll.HitTrack(point);
		if (horizontalScrollHovered_) {
			TouchHorizontalScrollReveal();
		}
		if (verticalScrollHovered_) {
			TouchVerticalScrollReveal();
		}
		if (horizontalScrollHovered_ || verticalScrollHovered_) {
			return CursorKind::Arrow;
		}
		return CursorKind::Arrow;
	}

	void DrawScrollBars(ID2D1DeviceContext* context, const Metrics& metrics) {
		if (metrics.renderHorizontal) {
			DrawRadixScrollbar(context, metrics.horizontalScroll, PrepareSharedBrush(context, SharedBrushSlot::Primary, accentColor_), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_), metrics.horizontalOpacity);
		}
		if (metrics.renderVertical) {
			DrawRadixScrollbar(context, metrics.verticalScroll, PrepareSharedBrush(context, SharedBrushSlot::Primary, accentColor_), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_), metrics.verticalOpacity);
		}
	}

	float HorizontalScrollOpacity() const {
		return AnimationSlotValue(L"horizontalScrollbarVisibility", 0.0f);
	}

	float VerticalScrollOpacity() const {
		return AnimationSlotValue(L"verticalScrollbarVisibility", 0.0f);
	}

	bool HorizontalScrollRevealActive() const {
		return horizontalScrollRevealUntil_ > std::chrono::steady_clock::now();
	}

	bool VerticalScrollRevealActive() const {
		return verticalScrollRevealUntil_ > std::chrono::steady_clock::now();
	}

	void TouchHorizontalScrollReveal() const {
		horizontalScrollRevealUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
	}

	void TouchVerticalScrollReveal() const {
		verticalScrollRevealUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
	}

	void UpdateScrollBarAnimations(const Metrics& metrics) {
		const bool horizontalVisible = metrics.showHorizontal && (horizontalScrollHovered_ || dragMode_ == DragMode::Horizontal || HorizontalScrollRevealActive());
		const bool verticalVisible = metrics.showVertical && (verticalScrollHovered_ || dragMode_ == DragMode::Vertical || VerticalScrollRevealActive());
		if (horizontalVisible != horizontalScrollBarVisibleTarget_) {
			horizontalScrollBarVisibleTarget_ = horizontalVisible;
			AnimateSlotLinear(L"horizontalScrollbarVisibility", horizontalVisible ? 1.0f : 0.0f, 0.16);
		}
		if (verticalVisible != verticalScrollBarVisibleTarget_) {
			verticalScrollBarVisibleTarget_ = verticalVisible;
			AnimateSlotLinear(L"verticalScrollbarVisibility", verticalVisible ? 1.0f : 0.0f, 0.16);
		}
	}

	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F mutedColor_{};
	float scrollX_ = 0.0f;
	float scrollY_ = 0.0f;
	mutable bool horizontalScrollHovered_ = false;
	mutable bool verticalScrollHovered_ = false;
	bool horizontalScrollBarVisibleTarget_ = false;
	bool verticalScrollBarVisibleTarget_ = false;
	mutable std::chrono::steady_clock::time_point horizontalScrollRevealUntil_{};
	mutable std::chrono::steady_clock::time_point verticalScrollRevealUntil_{};
	DragMode dragMode_ = DragMode::None;
	D2D1_POINT_2F dragOrigin_{ 0.0f, 0.0f };
	float originScrollX_ = 0.0f;
	float originScrollY_ = 0.0f;
	static constexpr float kScrollGutter = 18.0f;
};

class ImageButton final : public UIComponent {
public:
	ImageButton(std::wstring text,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& outlineColor,
		const D2D1_COLOR_F& textColor,
		std::function<void()> onClick)
		: text_(std::move(text)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), accentColor_(accentColor), outlineColor_(outlineColor), textColor_(textColor), onClick_(std::move(onClick)), textStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }
	CursorKind Cursor() const override { return CursorKind::Hand; }

	void SetTextAlignment(HorizontalAlign horizontal, VerticalAlign vertical) {
		horizontalAlign_ = horizontal;
		verticalAlign_ = vertical;
	}

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		textStyleOverride_ = true;
	}

	void Render(ID2D1DeviceContext* context) override {
		auto rounded = D2D1::RoundedRect(bounds_, 12.0f, 12.0f);
		context->FillRoundedRectangle(rounded, PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_, 1.0f - 0.12f * HoverProgress() - 0.08f * PressProgress()));
		context->DrawRoundedRectangle(rounded, PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.55f + 0.45f * FocusProgress()), 1.0f + FocusProgress());
		const D2D1_RECT_F imageRect = D2D1::RectF(bounds_.left + 12.0f, bounds_.top + 8.0f, bounds_.left + 48.0f, bounds_.bottom - 8.0f);
		context->FillRoundedRectangle(D2D1::RoundedRect(imageRect, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, accentColor_, 0.25f));
		auto* accentBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, accentColor_, 0.85f);
		context->DrawLine(D2D1::Point2F(imageRect.left + 8.0f, imageRect.bottom - 10.0f), D2D1::Point2F(imageRect.left + 18.0f, imageRect.top + 16.0f), accentBrush, 2.0f);
		context->DrawLine(D2D1::Point2F(imageRect.left + 18.0f, imageRect.top + 16.0f), D2D1::Point2F(imageRect.right - 8.0f, imageRect.bottom - 12.0f), accentBrush, 2.0f);
		context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(imageRect.right - 12.0f, imageRect.top + 12.0f), 4.0f, 4.0f), accentBrush);
		const D2D1_RECT_F textRect = D2D1::RectF(imageRect.right + 12.0f, bounds_.top + 6.0f, bounds_.right - 12.0f, bounds_.bottom - 6.0f);
		TextStyle effectiveStyle = textStyle_;
		effectiveStyle.horizontalAlign = horizontalAlign_;
		effectiveStyle.verticalAlign = verticalAlign_;
		effectiveStyle.color = textStyleOverride_ ? textStyle_.color : textColor_;
		auto* layout = layoutCache_.GetOrCreate(dwriteFactory_.Get(), format_.Get(), text_, textRect.right - textRect.left, textRect.bottom - textRect.top, &effectiveStyle);
		auto* styledBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, effectiveStyle.color);
		if (layout) {
			context->DrawTextLayout(D2D1::Point2F(textRect.left, textRect.top), layout, styledBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
		}
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
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F textColor_{};
	std::function<void()> onClick_;
	HorizontalAlign horizontalAlign_ = HorizontalAlign::Left;
	VerticalAlign verticalAlign_ = VerticalAlign::Center;
	TextStyle textStyle_{};
	bool textStyleOverride_ = false;
	mutable TextLayoutCache layoutCache_;
};

class Checkbox final : public UIComponent {
public:
	Checkbox(std::wstring text,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& textColor,
		const D2D1_COLOR_F& outlineColor,
		bool initialValue,
		std::function<void(bool)> onChanged)
		: text_(std::move(text)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), accentColor_(accentColor), textColor_(textColor), outlineColor_(outlineColor), checked_(initialValue), onChanged_(std::move(onChanged)), textStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		textStyleOverride_ = true;
	}

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(checkAnimation_, checked_ ? 1.0f : 0.0f);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		D2D1_RECT_F box = D2D1::RectF(bounds_.left, bounds_.top + 7.0f, bounds_.left + 22.0f, bounds_.top + 29.0f);
		context->FillRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		context->DrawRoundedRectangle(D2D1::RoundedRect(box, 6.0f, 6.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.55f + 0.45f * FocusProgress()), 1.0f + FocusProgress());
		const float check = checkAnimation_.Value();
		if (check > 0.01f) {
			context->FillRoundedRectangle(D2D1::RoundedRect(InflateRect(box, 3.0f), 4.0f, 4.0f), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, accentColor_, 0.45f + 0.35f * check + 0.2f * HoverProgress()));
		}
		DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, textStyleOverride_ ? textStyle_.color : textColor_), text_, D2D1::RectF(box.right + 12.0f, bounds_.top, bounds_.right, bounds_.bottom), textStyleOverride_ ? &textStyle_ : nullptr);
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
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F textColor_{};
	D2D1_COLOR_F outlineColor_{};
	TextStyle textStyle_{};
	bool textStyleOverride_ = false;
	bool checked_ = false;
	UIAnimation checkAnimation_{};
	std::function<void(bool)> onChanged_;
};

class RadioButton final : public UIComponent {
public:
	RadioButton(std::wstring text,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& textColor,
		const D2D1_COLOR_F& outlineColor,
		std::shared_ptr<int> selectedValue,
		int ownValue,
		std::function<void(int)> onChanged)
		: text_(std::move(text)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), accentColor_(accentColor), textColor_(textColor), outlineColor_(outlineColor), selectedValue_(std::move(selectedValue)), ownValue_(ownValue), onChanged_(std::move(onChanged)), textStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		textStyleOverride_ = true;
	}

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(selectionAnimation_, *selectedValue_ == ownValue_ ? 1.0f : 0.0f);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		Animate(selectionAnimation_, *selectedValue_ == ownValue_ ? 1.0f : 0.0f, 0.10);
		const float cx = bounds_.left + 11.0f;
		const float cy = bounds_.top + 18.0f;
		context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 10.0f, 10.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 10.0f, 10.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.55f + 0.45f * FocusProgress()), 1.0f + FocusProgress());
		const float selected = selectionAnimation_.Value();
		if (selected > 0.01f) {
			context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.0f + selected * 2.0f, 3.0f + selected * 2.0f), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, accentColor_, 0.5f + 0.3f * selected + 0.2f * HoverProgress()));
		}
		DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, textStyleOverride_ ? textStyle_.color : textColor_), text_, D2D1::RectF(bounds_.left + 30.0f, bounds_.top, bounds_.right, bounds_.bottom), textStyleOverride_ ? &textStyle_ : nullptr);
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

private:
	std::wstring text_;
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F textColor_{};
	D2D1_COLOR_F outlineColor_{};
	TextStyle textStyle_{};
	bool textStyleOverride_ = false;
	std::shared_ptr<int> selectedValue_;
	int ownValue_ = 0;
	UIAnimation selectionAnimation_{};
	std::function<void(int)> onChanged_;
};

class Slider final : public UIComponent {
public:
	Slider(std::wstring label,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& textColor,
		const D2D1_COLOR_F& outlineColor,
		float initialValue,
		std::function<void(float)> onChanged)
		: label_(std::move(label)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), accentColor_(accentColor), textColor_(textColor), outlineColor_(outlineColor), value_(Clamp01(initialValue)), onChanged_(std::move(onChanged)), textStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		textStyleOverride_ = true;
	}

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(valueAnimation_, value_);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, textStyleOverride_ ? textStyle_.color : textColor_), label_, D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f), textStyleOverride_ ? &textStyle_ : nullptr);
		D2D1_RECT_F track = TrackBounds();
		context->FillRoundedRectangle(D2D1::RoundedRect(track, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		const float value = valueAnimation_.Value();
		D2D1_RECT_F fill = D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * value, track.bottom);
		context->FillRoundedRectangle(D2D1::RoundedRect(fill, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_));
		context->DrawRoundedRectangle(D2D1::RoundedRect(track, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, outlineColor_, 0.45f + 0.55f * FocusProgress()), 1.0f + FocusProgress());
		float handleX = fill.right;
		const float handleRadius = 7.0f + HoverProgress() + PressProgress();
		context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(handleX, (track.top + track.bottom) * 0.5f), handleRadius, handleRadius), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, outlineColor_));
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); UpdateValue(point); }
	void OnPointerMove(D2D1_POINT_2F point) override { if (pressed_) UpdateValue(point); }
	void OnPointerUp(D2D1_POINT_2F point) override { if (pressed_) UpdateValue(point); UIComponent::OnPointerUp(point); }
	bool OnMouseWheel(int delta, D2D1_POINT_2F point) override {
		if (!PointInRect(bounds_, point)) {
			return false;
		}
		SetValue(value_ + (delta > 0 ? 0.03f : -0.03f));
		return true;
	}
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
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F textColor_{};
	D2D1_COLOR_F outlineColor_{};
	TextStyle textStyle_{};
	bool textStyleOverride_ = false;
	float value_ = 0.0f;
	UIAnimation valueAnimation_{};
	std::function<void(float)> onChanged_;
};

class ProgressBar final : public UIComponent {
public:
	ProgressBar(std::wstring label,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& textColor,
		UIAnimation* animation)
		: label_(std::move(label)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), accentColor_(accentColor), textColor_(textColor), animation_(animation), textStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		textStyleOverride_ = true;
	}

	void Render(ID2D1DeviceContext* context) override {
		std::wstringstream ss;
		float progress = Clamp01(animation_ ? animation_->Value() : 0.0f);
		ss << label_ << L"  " << static_cast<int>(progress * 100.0f) << L"%";
		auto text = ss.str();
		DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, textStyleOverride_ ? textStyle_.color : textColor_), text, D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f), textStyleOverride_ ? &textStyle_ : nullptr);
		D2D1_RECT_F track = D2D1::RectF(bounds_.left, bounds_.top + 20.0f, bounds_.right, bounds_.top + 32.0f);
		context->FillRoundedRectangle(D2D1::RoundedRect(track, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		D2D1_RECT_F fill = D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * progress, track.bottom);
		context->FillRoundedRectangle(D2D1::RoundedRect(fill, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_));
	}

private:
	std::wstring label_;
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F textColor_{};
	UIAnimation* animation_ = nullptr;
	TextStyle textStyle_{};
	bool textStyleOverride_ = false;
};

class TextInput final : public UIComponent {
public:
	TextInput(std::wstring label,
		std::wstring placeholder,
		std::wstring initialText,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& outlineColor,
		const D2D1_COLOR_F& textColor,
		const D2D1_COLOR_F& mutedColor,
		const D2D1_COLOR_F& selectionColor,
		bool multiline,
		std::function<void(std::wstring_view)> onChanged)
		: label_(std::move(label)), placeholder_(std::move(placeholder)), text_(std::move(initialText)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), outlineColor_(outlineColor), textColor_(textColor), mutedColor_(mutedColor), selectionColor_(selectionColor), multiline_(multiline), onChanged_(std::move(onChanged)), textStyle_(CaptureTextStyle(format_.Get(), textColor_)), labelStyle_(CaptureTextStyle(format_.Get(), mutedColor_)) {
		caret_ = text_.size();
		selectionAnchor_ = caret_;
	}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }
	CursorKind Cursor() const override { return CursorKind::IBeam; }
	bool WantsFrameTick() const override {
		return HorizontalScrollRevealActive() || VerticalScrollRevealActive();
	}
	CursorKind CursorAt(D2D1_POINT_2F point) const override {
		auto visual = ComputeVisualState();
		horizontalScrollHovered_ = visual.renderHorizontalScroll && visual.horizontalScroll.HitTrack(point);
		verticalScrollHovered_ = visual.renderVerticalScroll && visual.verticalScroll.HitTrack(point);
		if (horizontalScrollHovered_) {
			TouchHorizontalScrollReveal();
		}
		if (verticalScrollHovered_) {
			TouchVerticalScrollReveal();
		}
		if (horizontalScrollHovered_ || verticalScrollHovered_) {
			return CursorKind::Arrow;
		}
		return PointInRect(visual.box, point) ? CursorKind::IBeam : CursorKind::Arrow;
	}

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		styleOverride_ = true;
	}

	void SetLabelStyle(const TextStyle& style) {
		labelStyle_ = style;
		labelStyleOverride_ = true;
	}

	void SetStyledRanges(std::vector<StyledTextRange> ranges) {
		styledRanges_ = std::move(ranges);
	}

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(caretOpacityAnimation_, 1.0f);
			animator_->Attach(horizontalScrollVisibilityAnimation_, 0.0f);
			animator_->Attach(verticalScrollVisibilityAnimation_, 0.0f);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		UpdateCaretBlink();
		auto visual = ComputeVisualState();
		float horizontalScrollOpacity = visual.horizontalOpacity;
		float verticalScrollOpacity = visual.verticalOpacity;
		if (!multiline_ && HorizontalOverflowHintActive() && horizontalScrollOpacity < 0.01f) {
			horizontalScrollOpacity = 1.0f;
		}
		if (multiline_ && VerticalOverflowHintActive() && verticalScrollOpacity < 0.01f) {
			verticalScrollOpacity = 1.0f;
		}
		auto* textBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, styleOverride_ ? textStyle_.color : textColor_);
		auto* mutedBrush = PrepareSharedBrush(context, SharedBrushSlot::Secondary, mutedColor_);
		auto* surfaceBrush = PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_);
		auto* outlineBrush = PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_);
		auto* selectionBrush = PrepareSharedBrush(context, SharedBrushSlot::Primary, selectionColor_);
		const D2D1_RECT_F labelRect = D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f);
		auto* labelLayout = labelLayoutCache_.GetOrCreate(dwriteFactory_.Get(), format_.Get(), label_, labelRect.right - labelRect.left, labelRect.bottom - labelRect.top, labelStyleOverride_ ? &labelStyle_ : nullptr);
		auto* labelBrush = PrepareSharedBrush(context, SharedBrushSlot::Secondary, labelStyleOverride_ ? labelStyle_.color : mutedColor_);
		auto* styledTextBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, styleOverride_ ? textStyle_.color : textColor_);
		if (labelLayout) {
			context->DrawTextLayout(D2D1::Point2F(labelRect.left, labelRect.top), labelLayout, labelBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
		}
		D2D1_RECT_F box = TextBounds();
		if (focused_) {
			context->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left - 1.0f, box.top - 1.0f, box.right + 1.0f, box.bottom + 1.0f), 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, selectionColor_, 0.10f));
		}
		context->FillRoundedRectangle(D2D1::RoundedRect(box, 12.0f, 12.0f), surfaceBrush);
		context->DrawRoundedRectangle(D2D1::RoundedRect(box, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.58f + FocusProgress() * 0.42f), focused_ ? 1.8f : 1.0f);
		UpdateScrollbarVisibilityAnimations(visual);
		SyncScrollOffsets(visual.layout, visual.content);
		if (!multiline_) {
			const float visibleWidth = (std::max)(1.0f, visual.content.right - visual.content.left);
			const float drawMaxScrollX = (std::max)(0.0f, MeasureSingleLineContentWidth(visual.renderText) - visibleWidth + 4.0f);
			scrollX_ = (std::clamp)(scrollX_, 0.0f, drawMaxScrollX);
		}
		if (visual.renderHorizontalScroll) {
			visual.horizontalScroll.offset = scrollX_;
		}
		if (visual.renderVerticalScroll) {
			visual.verticalScroll.offset = scrollY_;
		}
		float originY = visual.content.top - scrollY_;
		if (!multiline_) {
			const float visibleHeight = (std::max)(1.0f, visual.content.bottom - visual.content.top);
			const float textHeight = (std::max)(visual.metrics.height, detail::kLineHeight);
			originY = visual.content.top + (std::max)(0.0f, std::floor((visibleHeight - textHeight) * 0.5f)) - scrollY_;
		}
		const D2D1_POINT_2F origin = D2D1::Point2F(visual.content.left - scrollX_, originY);
		context->PushAxisAlignedClip(visual.content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		DrawSelection(context, visual.layout, origin, selectionBrush);
		if (visual.renderText.empty()) {
			D2D1_RECT_F placeholderRect = visual.content;
			if (!multiline_) {
				const float textHeight = detail::kLineHeight + 2.0f;
				const float top = visual.content.top + (std::max)(0.0f, std::floor(((visual.content.bottom - visual.content.top) - textHeight) * 0.5f));
				placeholderRect = D2D1::RectF(visual.content.left, top, visual.content.right, top + textHeight);
			}
			context->DrawTextW(placeholder_.c_str(), static_cast<UINT32>(placeholder_.size()), format_.Get(), placeholderRect, mutedBrush);
		}
		else {
			if (multiline_) {
				context->DrawTextLayout(origin, visual.layout, styledTextBrush, D2D1_DRAW_TEXT_OPTIONS_NONE);
			}
			else {
				const float textHeight = (std::max)(detail::kLineHeight + 2.0f, visual.metrics.height);
				const float textWidth = MeasureSingleLineContentWidth(visual.renderText) + 24.0f;
				const D2D1_RECT_F textRect = D2D1::RectF(origin.x, origin.y, origin.x + textWidth, origin.y + textHeight);
				context->DrawTextW(visual.renderText.c_str(), static_cast<UINT32>(visual.renderText.size()), format_.Get(), textRect, styledTextBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
			}
			DrawImeUnderline(context, visual.layout, origin, PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_));
		}
		if (focused_ && caretOpacityAnimation_.Value() > 0.15f) {
			DrawCaret(context, visual.layout, origin, styleOverride_ ? textStyle_.color : textColor_, caretOpacityAnimation_.Value());
		}
		context->PopAxisAlignedClip();
		if (visual.renderHorizontalScroll) {
			DrawScrollBar(context, visual.horizontalScroll, horizontalScrollOpacity);
		}
		if (multiline_) {
			const bool forceVerticalCue = scrollY_ > 0.5f || VerticalOverflowHintActive();
			if (visual.renderVerticalScroll || forceVerticalCue) {
				auto verticalScroll = visual.verticalScroll;
				float effectiveVerticalOpacity = verticalScrollOpacity;
				if (forceVerticalCue && effectiveVerticalOpacity < 0.01f) {
					effectiveVerticalOpacity = VerticalOverflowHintActive() ? 1.0f : 0.85f;
				}
				if (!verticalScroll.Visible()) {
					const float fallbackMaxScrollY = (std::max)((std::max)(visual.maxScrollY, verticalHintMaxScroll_), scrollY_ + 1.0f);
					verticalScroll = MakeVerticalScrollState(visual.box, fallbackMaxScrollY, visual.content.bottom - visual.content.top);
					verticalScroll.offset = (std::clamp)(scrollY_, 0.0f, fallbackMaxScrollY);
				}
				DrawScrollBar(context, verticalScroll, effectiveVerticalOpacity);
			}
		}
		else if (visual.renderVerticalScroll) {
			DrawScrollBar(context, visual.verticalScroll, verticalScrollOpacity);
		}
	}

	void OnPointerDown(D2D1_POINT_2F point) override {
		auto visual = ComputeVisualState();
		if (visual.showHorizontalScroll && visual.horizontalScroll.HitTrack(point)) {
			UIComponent::OnPointerDown(point);
			scrollDragMode_ = ScrollDragMode::Horizontal;
			horizontalScrollHovered_ = true;
			TouchHorizontalScrollReveal();
			ensureCaretVisible_ = false;
			dragOrigin_ = point;
			originScrollX_ = scrollX_;
			if (!visual.horizontalScroll.HitThumb(point)) {
				scrollX_ = (std::clamp)(ScrollbarOffsetForPointer(visual.horizontalScroll, point.x), 0.0f, visual.maxScrollX);
				originScrollX_ = scrollX_;
			}
			ResetCaretBlink();
			return;
		}
		if (visual.showVerticalScroll && visual.verticalScroll.HitTrack(point)) {
			UIComponent::OnPointerDown(point);
			scrollDragMode_ = ScrollDragMode::Vertical;
			verticalScrollHovered_ = true;
			TouchVerticalScrollReveal();
			ensureCaretVisible_ = false;
			dragOrigin_ = point;
			originScrollY_ = scrollY_;
			if (!visual.verticalScroll.HitThumb(point)) {
				scrollY_ = (std::clamp)(ScrollbarOffsetForPointer(visual.verticalScroll, point.y), 0.0f, visual.maxScrollY);
				originScrollY_ = scrollY_;
			}
			ResetCaretBlink();
			return;
		}
		UIComponent::OnPointerDown(point);
		draggingSelection_ = true;
		const size_t index = HitTestText(point);
		caret_ = index;
		selectionAnchor_ = index;
		desiredCaretX_.reset();
		ensureCaretVisible_ = true;
		ResetCaretBlink();
	}

	void OnPointerMove(D2D1_POINT_2F point) override {
		if (scrollDragMode_ != ScrollDragMode::None && pressed_) {
			auto visual = ComputeVisualState();
			if (scrollDragMode_ == ScrollDragMode::Horizontal && visual.showHorizontalScroll) {
				TouchHorizontalScrollReveal();
				const auto track = visual.horizontalScroll.TrackBounds();
				const auto thumb = visual.horizontalScroll.ThumbBounds();
				const float travel = (std::max)(1.0f, (track.right - track.left) - (thumb.right - thumb.left));
				scrollX_ = (std::clamp)(originScrollX_ + ((point.x - dragOrigin_.x) / travel) * visual.maxScrollX, 0.0f, visual.maxScrollX);
				ensureCaretVisible_ = false;
				return;
			}
			if (scrollDragMode_ == ScrollDragMode::Vertical && visual.showVerticalScroll) {
				TouchVerticalScrollReveal();
				const auto track = visual.verticalScroll.TrackBounds();
				const auto thumb = visual.verticalScroll.ThumbBounds();
				const float travel = (std::max)(1.0f, (track.bottom - track.top) - (thumb.bottom - thumb.top));
				scrollY_ = (std::clamp)(originScrollY_ + ((point.y - dragOrigin_.y) / travel) * visual.maxScrollY, 0.0f, visual.maxScrollY);
				ensureCaretVisible_ = false;
				return;
			}
		}
		if (draggingSelection_) {
			caret_ = HitTestText(point);
			desiredCaretX_.reset();
			ensureCaretVisible_ = true;
			ResetCaretBlink();
		}
	}

	void OnPointerUp(D2D1_POINT_2F point) override {
		if (scrollDragMode_ != ScrollDragMode::None) {
			horizontalScrollHovered_ = false;
			verticalScrollHovered_ = false;
			scrollDragMode_ = ScrollDragMode::None;
			UIComponent::OnPointerUp(point);
			return;
		}
		UIComponent::OnPointerUp(point);
		draggingSelection_ = false;
	}

	void OnHover(bool hovered) override {
		UIComponent::OnHover(hovered);
		if (!hovered && scrollDragMode_ == ScrollDragMode::None) {
			horizontalScrollHovered_ = false;
			verticalScrollHovered_ = false;
		}
	}

	bool OnMouseWheel(int delta, D2D1_POINT_2F point) override {
		auto visual = ComputeVisualState();
		if (!PointInRect(visual.box, point)) {
			return false;
		}
		if (multiline_) {
			if (!visual.showVerticalScroll) {
				return false;
			}
			const float previous = scrollY_;
			scrollY_ = (std::clamp)(scrollY_ + (delta > 0 ? -28.0f : 28.0f), 0.0f, visual.maxScrollY);
			ensureCaretVisible_ = false;
			const bool changed = !NearlyEqual(previous, scrollY_, 0.05f);
			if (changed) {
				TouchVerticalScrollReveal();
			}
			return changed;
		}
		if (!visual.showHorizontalScroll) {
			return false;
		}
		const float previous = scrollX_;
		scrollX_ = (std::clamp)(scrollX_ + (delta > 0 ? -32.0f : 32.0f), 0.0f, visual.maxScrollX);
		ensureCaretVisible_ = false;
		const bool changed = !NearlyEqual(previous, scrollX_, 0.05f);
		if (changed) {
			TouchHorizontalScrollReveal();
		}
		return changed;
	}

	void OnKeyDown(WPARAM key, const KeyModifiers& modifiers) override {
		if (modifiers.ctrl) {
			switch (key) {
			case 'A':
			case 'a':
				selectionAnchor_ = 0;
				caret_ = text_.size();
				desiredCaretX_.reset();
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
				desiredCaretX_.reset();
				NotifyChanged();
			}
			break;
		case VK_DELETE:
			if (DeleteSelection()) {
				return;
			}
			if (caret_ < text_.size()) {
				text_.erase(caret_, 1);
				desiredCaretX_.reset();
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
				MoveCaret(MoveVertical(-1), modifiers.shift, true);
			}
			break;
		case VK_DOWN:
			if (multiline_) {
				MoveCaret(MoveVertical(1), modifiers.shift, true);
			}
			break;
		default:
			break;
		}
		ensureCaretVisible_ = true;
		ResetCaretBlink();
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
			scrollDragMode_ = ScrollDragMode::None;
			AnimateLinear(caretOpacityAnimation_, 0.0f, 0.08);
			caretTargetVisible_ = false;
			return;
		}
		ensureCaretVisible_ = true;
		ResetCaretBlink();
	}

	void OnImeStart(HWND) override {
		imeActive_ = true;
		compositionAnchor_ = SelectionStart();
		compositionReplaceLength_ = HasSelection() ? SelectionLength() : 0;
		imeComposition_.clear();
		imeCursorPos_ = 0;
		ResetCaretBlink();
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
		ensureCaretVisible_ = true;
		ResetCaretBlink();
	}

	void OnImeEnd(HWND) override {
		imeActive_ = false;
		imeComposition_.clear();
		compositionReplaceLength_ = 0;
		ResetCaretBlink();
	}

	const std::wstring& Text() const { return text_; }

private:
	static constexpr float kScrollGutter = 18.0f;
	static constexpr float kSingleLineScrollGutter = 12.0f;

	enum class ScrollDragMode {
		None,
		Horizontal,
		Vertical,
	};

	struct VisualState {
		std::wstring renderText;
		D2D1_RECT_F box{ D2D1::RectF() };
		D2D1_RECT_F content{ D2D1::RectF() };
		IDWriteTextLayout* layout = nullptr;
		DWRITE_TEXT_METRICS metrics{};
		float maxScrollX = 0.0f;
		float maxScrollY = 0.0f;
		bool showHorizontalScroll = false;
		bool showVerticalScroll = false;
		bool renderHorizontalScroll = false;
		bool renderVerticalScroll = false;
		float horizontalOpacity = 0.0f;
		float verticalOpacity = 0.0f;
		ScrollbarState horizontalScroll{ ScrollOrientation::Horizontal };
		ScrollbarState verticalScroll{ ScrollOrientation::Vertical };
	};

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

	float MeasureMultilineContentHeight(IDWriteTextLayout* layout, const DWRITE_TEXT_METRICS& metrics) const {
		if (!layout) {
			return 0.0f;
		}
		std::array<DWRITE_LINE_METRICS, 16> stackMetrics{};
		UINT32 actualCount = 0;
		HRESULT hr = layout->GetLineMetrics(stackMetrics.data(), static_cast<UINT32>(stackMetrics.size()), &actualCount);
		if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || hr == E_NOT_SUFFICIENT_BUFFER) {
			std::vector<DWRITE_LINE_METRICS> lineMetrics(actualCount);
			hr = layout->GetLineMetrics(lineMetrics.data(), actualCount, &actualCount);
			if (SUCCEEDED(hr)) {
				float totalHeight = 0.0f;
				for (UINT32 index = 0; index < actualCount; ++index) {
					totalHeight += lineMetrics[index].height;
				}
				return (std::max)(totalHeight, metrics.height);
			}
		}
		else if (SUCCEEDED(hr)) {
			float totalHeight = 0.0f;
			for (UINT32 index = 0; index < actualCount; ++index) {
				totalHeight += stackMetrics[index].height;
			}
			return (std::max)(totalHeight, metrics.height);
		}
		return metrics.height;
	}

	VisualState ComputeVisualState() const {
		VisualState state;
		state.renderText = DisplayText();
		state.box = TextBounds();
		state.horizontalOpacity = multiline_ ? 0.0f : horizontalScrollVisibilityAnimation_.Value();
		state.verticalOpacity = multiline_ ? verticalScrollVisibilityAnimation_.Value() : 0.0f;
		bool reserveHorizontalScroll = !multiline_ && (state.horizontalOpacity > 0.01f || HorizontalOverflowHintActive());
		bool reserveVerticalScroll = multiline_ && (state.verticalOpacity > 0.01f || VerticalOverflowHintActive());

		auto refreshLayout = [&]() {
			state.content = multiline_
				? InflateRect(state.box, 12.0f)
				: D2D1::RectF(state.box.left + 12.0f, state.box.top + 6.0f, state.box.right - 12.0f, state.box.bottom - 6.0f);
			if (reserveVerticalScroll) {
				state.content.right -= kScrollGutter;
			}
			if (reserveHorizontalScroll) {
				state.content.bottom -= kSingleLineScrollGutter;
			}
			state.layout = CreateLayout(state.renderText, state.content);
			state.metrics = {};
			if (state.layout) {
				state.layout->GetMetrics(&state.metrics);
			}
		};

		refreshLayout();
		if (multiline_) {
			const float contentHeight = MeasureMultilineContentHeight(state.layout, state.metrics);
			state.maxScrollY = (std::max)(0.0f, contentHeight - (state.content.bottom - state.content.top) + 6.0f);
			state.showVerticalScroll = state.maxScrollY > 0.5f;
			if (state.showVerticalScroll != reserveVerticalScroll) {
				reserveVerticalScroll = state.showVerticalScroll || state.verticalOpacity > 0.01f;
				refreshLayout();
				const float reservedContentHeight = MeasureMultilineContentHeight(state.layout, state.metrics);
				state.maxScrollY = (std::max)(0.0f, reservedContentHeight - (state.content.bottom - state.content.top) + 6.0f);
			}
			state.showVerticalScroll = state.maxScrollY > 0.5f;
			UpdateVerticalOverflowCue(state.showVerticalScroll, state.maxScrollY);
			const float hintedMaxScrollY = VerticalOverflowHintActive() ? (std::max)(state.maxScrollY, verticalHintMaxScroll_) : state.maxScrollY;
			state.renderVerticalScroll = state.showVerticalScroll || VerticalOverflowHintActive() || state.verticalOpacity > 0.01f;
			if (state.renderVerticalScroll) {
				const float effectiveMaxScrollY = (std::max)(1.0f, hintedMaxScrollY);
				state.verticalScroll = MakeVerticalScrollState(state.box, effectiveMaxScrollY, state.content.bottom - state.content.top);
				if (!state.showVerticalScroll) {
					state.verticalScroll.offset = 0.0f;
				}
			}
		}
		else {
			float contentWidth = MeasureSingleLineContentWidth(state.renderText);
			state.maxScrollX = (std::max)(0.0f, contentWidth - (state.content.right - state.content.left) + 8.0f);
			state.showHorizontalScroll = state.maxScrollX > 0.5f;
			if (state.showHorizontalScroll != reserveHorizontalScroll) {
				reserveHorizontalScroll = state.showHorizontalScroll || state.horizontalOpacity > 0.01f;
				refreshLayout();
				contentWidth = MeasureSingleLineContentWidth(state.renderText);
				state.maxScrollX = (std::max)(0.0f, contentWidth - (state.content.right - state.content.left) + 8.0f);
			}
			state.showHorizontalScroll = state.maxScrollX > 0.5f;
			UpdateHorizontalOverflowCue(state.showHorizontalScroll, state.maxScrollX);
			const float hintedMaxScrollX = HorizontalOverflowHintActive() ? (std::max)(state.maxScrollX, horizontalHintMaxScroll_) : state.maxScrollX;
			state.renderHorizontalScroll = state.showHorizontalScroll || HorizontalOverflowHintActive() || state.horizontalOpacity > 0.01f;
			if (state.renderHorizontalScroll) {
				const float effectiveMaxScrollX = (std::max)(1.0f, hintedMaxScrollX);
				state.horizontalScroll = MakeHorizontalScrollState(state.box, effectiveMaxScrollX, state.content.right - state.content.left);
				if (!state.showHorizontalScroll) {
					state.horizontalScroll.offset = 0.0f;
				}
			}
		}

		return state;
	}

	void UpdateScrollbarVisibilityAnimations(const VisualState& visual) {
		if (!multiline_) {
			AnimateLinear(horizontalScrollVisibilityAnimation_, visual.renderHorizontalScroll && (horizontalScrollHovered_ || scrollDragMode_ == ScrollDragMode::Horizontal || HorizontalScrollRevealActive()) ? 1.0f : 0.0f, 0.16);
		}
		if (multiline_) {
			AnimateLinear(verticalScrollVisibilityAnimation_, visual.renderVerticalScroll && (verticalScrollHovered_ || scrollDragMode_ == ScrollDragMode::Vertical || VerticalScrollRevealActive()) ? 1.0f : 0.0f, 0.16);
		}
	}

	bool HorizontalOverflowHintActive() const {
		return horizontalHintMaxScroll_ > 0.5f && HorizontalScrollRevealActive();
	}

	bool VerticalOverflowHintActive() const {
		return verticalHintMaxScroll_ > 0.5f && VerticalScrollRevealActive();
	}

	void UpdateHorizontalOverflowCue(bool overflowing, float maxScroll) const {
		if (overflowing) {
			lastHorizontalMaxScroll_ = maxScroll;
		}
		if (overflowing != previousHorizontalOverflow_) {
			horizontalHintMaxScroll_ = overflowing ? maxScroll : lastHorizontalMaxScroll_;
			TouchHorizontalScrollReveal();
			previousHorizontalOverflow_ = overflowing;
		}
		else if (overflowing) {
			horizontalHintMaxScroll_ = maxScroll;
		}
	}

	void UpdateVerticalOverflowCue(bool overflowing, float maxScroll) const {
		if (overflowing) {
			lastVerticalMaxScroll_ = maxScroll;
		}
		if (overflowing != previousVerticalOverflow_) {
			verticalHintMaxScroll_ = overflowing ? maxScroll : lastVerticalMaxScroll_;
			TouchVerticalScrollReveal();
			previousVerticalOverflow_ = overflowing;
		}
		else if (overflowing) {
			verticalHintMaxScroll_ = maxScroll;
		}
	}

	bool HorizontalScrollRevealActive() const {
		return horizontalScrollRevealUntil_ > std::chrono::steady_clock::now();
	}

	bool VerticalScrollRevealActive() const {
		return verticalScrollRevealUntil_ > std::chrono::steady_clock::now();
	}

	void TouchHorizontalScrollReveal() const {
		horizontalScrollRevealUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
	}

	void TouchVerticalScrollReveal() const {
		verticalScrollRevealUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
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

	IDWriteTextLayout* CreateLayout(std::wstring_view renderText, const D2D1_RECT_F& content) const {
		const float width = multiline_ ? (content.right - content.left) : (std::max)(content.right - content.left, MeasureSingleLineContentWidth(renderText) + 24.0f);
		const float height = multiline_ ? 32768.0f : (content.bottom - content.top);
		return contentLayoutCache_.GetOrCreate(dwriteFactory_.Get(), format_.Get(), renderText, width, height, styleOverride_ ? &textStyle_ : nullptr, styledRanges_, multiline_ ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
	}

	float MeasureSingleLineContentWidth(std::wstring_view renderText) const {
		const auto metrics = MeasureTextMetrics(dwriteFactory_.Get(), format_.Get(), renderText, styleOverride_ ? &textStyle_ : nullptr, DWRITE_WORD_WRAPPING_NO_WRAP);
		return (std::max)(1.0f, metrics.widthIncludingTrailingWhitespace + 2.0f);
	}

	void ShiftStyledRanges(size_t start, ptrdiff_t delta, size_t removedLength = 0) {
		std::vector<StyledTextRange> updated;
		updated.reserve(styledRanges_.size());
		for (auto range : styledRanges_) {
			const size_t rangeEnd = range.start + range.length;
			if (removedLength > 0 && rangeEnd <= start) {
				updated.push_back(range);
				continue;
			}
			if (removedLength > 0 && range.start >= start + removedLength) {
				range.start = static_cast<size_t>(static_cast<ptrdiff_t>(range.start) + delta);
				updated.push_back(range);
				continue;
			}
			if (removedLength > 0) {
				const size_t newStart = range.start < start ? range.start : start;
				const size_t preservedPrefix = range.start < start ? start - range.start : 0;
				const size_t preservedSuffix = rangeEnd > start + removedLength ? rangeEnd - (start + removedLength) : 0;
				range.start = newStart;
				range.length = preservedPrefix + preservedSuffix;
				if (range.length > 0) {
					updated.push_back(range);
				}
				continue;
			}
			if (range.start >= start) {
				range.start = static_cast<size_t>(static_cast<ptrdiff_t>(range.start) + delta);
			}
			else if (rangeEnd > start) {
				range.length = static_cast<size_t>(static_cast<ptrdiff_t>(range.length) + delta);
			}
			updated.push_back(range);
		}
		styledRanges_ = std::move(updated);
	}

	void DrawSelection(ID2D1DeviceContext* context, IDWriteTextLayout* layout, D2D1_POINT_2F origin, ID2D1SolidColorBrush* selectionBrush) {
		if (!layout || imeActive_ || !HasSelection()) {
			return;
		}
		std::array<DWRITE_HIT_TEST_METRICS, 8> stackMetrics{};
		UINT32 actualCount = 0;
		const UINT32 length = static_cast<UINT32>(SelectionLength());
		HRESULT hr = layout->HitTestTextRange(static_cast<UINT32>(SelectionStart()), length, origin.x, origin.y, stackMetrics.data(), static_cast<UINT32>(stackMetrics.size()), &actualCount);
		std::vector<DWRITE_HIT_TEST_METRICS> dynamicMetrics;
		if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
			dynamicMetrics.resize(actualCount);
			if (SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(SelectionStart()), length, origin.x, origin.y, dynamicMetrics.data(), actualCount, &actualCount))) {
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

	void DrawImeUnderline(ID2D1DeviceContext* context, IDWriteTextLayout* layout, D2D1_POINT_2F origin, ID2D1SolidColorBrush* outlineBrush) {
		if (!layout || !imeActive_ || imeComposition_.empty()) {
			return;
		}
		std::array<DWRITE_HIT_TEST_METRICS, 8> stackMetrics{};
		UINT32 actualCount = 0;
		const UINT32 start = static_cast<UINT32>(compositionAnchor_);
		const UINT32 length = static_cast<UINT32>(imeComposition_.size());
		HRESULT hr = layout->HitTestTextRange(start, length, origin.x, origin.y, stackMetrics.data(), static_cast<UINT32>(stackMetrics.size()), &actualCount);
		if (FAILED(hr)) {
			return;
		}
		for (UINT32 index = 0; index < actualCount; ++index) {
			const auto& metric = stackMetrics[index];
			context->DrawLine(D2D1::Point2F(metric.left, metric.top + metric.height + 1.0f), D2D1::Point2F(metric.left + metric.width, metric.top + metric.height + 1.0f), outlineBrush, 1.0f);
		}
	}

	void DrawCaret(ID2D1DeviceContext* context, IDWriteTextLayout* layout, D2D1_POINT_2F origin, const D2D1_COLOR_F& caretColor, float opacity) {
		FLOAT x = origin.x;
		FLOAT y = origin.y;
		DWRITE_HIT_TEST_METRICS metrics{};
		if (!layout || FAILED(layout->HitTestTextPosition(static_cast<UINT32>(DisplayCaret()), FALSE, &x, &y, &metrics))) {
			metrics.height = multiline_ ? 18.0f : detail::kLineHeight;
			x = origin.x + 1.0f;
			y = origin.y + 2.0f;
		}
		else {
			x += origin.x;
			y += origin.y;
		}
		auto* caretBrush = PrepareSharedBrush(context, SharedBrushSlot::Tertiary, caretColor, opacity);
		if (!caretBrush) {
			return;
		}
		context->FillRectangle(D2D1::RectF(x, y, x + 1.0f, y + (std::max)(metrics.height, 18.0f)), caretBrush);
	}

	void SyncScrollOffsets(IDWriteTextLayout* layout, const D2D1_RECT_F& content) {
		if (!layout) {
			return;
		}
		DWRITE_TEXT_METRICS textMetrics{};
		layout->GetMetrics(&textMetrics);
		const float visibleWidth = (std::max)(1.0f, content.right - content.left);
		const float visibleHeight = (std::max)(1.0f, content.bottom - content.top);
		const float contentWidth = multiline_ ? 0.0f : MeasureSingleLineContentWidth(DisplayText());
		const float contentHeight = multiline_ ? MeasureMultilineContentHeight(layout, textMetrics) : 0.0f;
		const float maxScrollX = multiline_ ? 0.0f : (std::max)(0.0f, contentWidth - visibleWidth + 8.0f);
		const float maxScrollY = multiline_ ? (std::max)(0.0f, contentHeight - visibleHeight + 6.0f) : 0.0f;
		scrollX_ = (std::clamp)(scrollX_, 0.0f, maxScrollX);
		scrollY_ = (std::clamp)(scrollY_, 0.0f, maxScrollY);
		if (!ensureCaretVisible_) {
			return;
		}
		FLOAT caretX = 0.0f;
		FLOAT caretY = 0.0f;
		DWRITE_HIT_TEST_METRICS caretMetrics{};
		if (SUCCEEDED(layout->HitTestTextPosition(static_cast<UINT32>(DisplayCaret()), FALSE, &caretX, &caretY, &caretMetrics))) {
			if (caretX < scrollX_ + 4.0f) {
				scrollX_ = (std::max)(0.0f, caretX - 4.0f);
			}
			else if (caretX + 2.0f > scrollX_ + visibleWidth - 4.0f) {
				scrollX_ = (std::min)(maxScrollX, caretX + 2.0f - visibleWidth + 4.0f);
			}
			if (caretY < scrollY_ + 2.0f) {
				scrollY_ = (std::max)(0.0f, caretY - 2.0f);
			}
			else if (caretY + caretMetrics.height > scrollY_ + visibleHeight - 4.0f) {
				scrollY_ = (std::min)(maxScrollY, caretY + caretMetrics.height - visibleHeight + 4.0f);
			}
		}
		scrollX_ = (std::clamp)(scrollX_, 0.0f, maxScrollX);
		scrollY_ = (std::clamp)(scrollY_, 0.0f, maxScrollY);
		ensureCaretVisible_ = false;
	}

	ScrollbarState MakeHorizontalScrollState(const D2D1_RECT_F& box, float maxScrollX, float visibleWidth) const {
		ScrollbarState state{ ScrollOrientation::Horizontal };
		state.viewport = box;
		state.contentExtent = (box.right - box.left) + (std::max)(0.0f, maxScrollX);
		state.offset = scrollX_;
		state.inset = 12.0f;
		state.edgePadding = 2.0f;
		state.hovered = horizontalScrollHovered_ || scrollDragMode_ == ScrollDragMode::Horizontal;
		state.dragging = scrollDragMode_ == ScrollDragMode::Horizontal;
		return state;
	}

	ScrollbarState MakeVerticalScrollState(const D2D1_RECT_F& box, float maxScrollY, float visibleHeight) const {
		ScrollbarState state{ ScrollOrientation::Vertical };
		state.viewport = box;
		state.contentExtent = (box.bottom - box.top) + (std::max)(0.0f, maxScrollY);
		state.offset = scrollY_;
		state.inset = 12.0f;
		state.edgePadding = 2.0f;
		state.hovered = verticalScrollHovered_ || scrollDragMode_ == ScrollDragMode::Vertical;
		state.dragging = scrollDragMode_ == ScrollDragMode::Vertical;
		return state;
	}

	void DrawScrollBar(ID2D1DeviceContext* context, const ScrollbarState& state, float opacity) {
		DrawRadixScrollbar(context, state, PrepareSharedBrush(context, SharedBrushSlot::Primary, selectionColor_), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_), opacity);
	}

	void UpdateCaretBlink() {
		if (!focused_) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now >= nextCaretBlinkToggle_) {
			caretTargetVisible_ = !caretTargetVisible_;
			AnimateLinear(caretOpacityAnimation_, caretTargetVisible_ ? 1.0f : 0.0f, 0.12);
			nextCaretBlinkToggle_ = now + std::chrono::milliseconds(530);
		}
	}

	void ResetCaretBlink() {
		caretTargetVisible_ = true;
		AnimateLinear(caretOpacityAnimation_, 1.0f, 0.06);
		nextCaretBlinkToggle_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
	}

	size_t HitTestText(D2D1_POINT_2F point) const {
		const auto visual = ComputeVisualState();
		if (!visual.layout) {
			return text_.size();
		}
		BOOL trailing = FALSE;
		BOOL inside = FALSE;
		DWRITE_HIT_TEST_METRICS metrics{};
		const float localX = point.x - visual.content.left + scrollX_;
		const float localY = point.y - visual.content.top + scrollY_;
		if (FAILED(visual.layout->HitTestPoint(localX, localY, &trailing, &inside, &metrics))) {
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
		ShiftStyledRanges(start, -static_cast<ptrdiff_t>(SelectionLength()), SelectionLength());
		text_.erase(start, SelectionLength());
		caret_ = start;
		selectionAnchor_ = start;
		ensureCaretVisible_ = true;
		NotifyChanged();
		return true;
	}

	void ReplaceRange(size_t start, size_t length, std::wstring_view replacement) {
		if (length > 0) {
			ShiftStyledRanges(start, -static_cast<ptrdiff_t>(length), length);
		}
		if (!replacement.empty()) {
			ShiftStyledRanges(start, static_cast<ptrdiff_t>(replacement.size()));
		}
		text_.replace(start, length, replacement.data(), replacement.size());
		caret_ = start + replacement.size();
		selectionAnchor_ = caret_;
		ensureCaretVisible_ = true;
		ResetCaretBlink();
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
		desiredCaretX_.reset();
	}

	void MoveCaret(size_t index, bool extendSelection, bool preserveDesiredColumn = false) {
		caret_ = (std::min)(index, text_.size());
		if (!extendSelection) {
			selectionAnchor_ = caret_;
		}
		if (!preserveDesiredColumn) {
			desiredCaretX_.reset();
		}
		ensureCaretVisible_ = true;
		ResetCaretBlink();
	}

	size_t MoveVerticalByLineBreak(int direction) const {
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

	size_t MoveVertical(int direction) const {
		const auto visual = ComputeVisualState();
		if (!visual.layout) {
			return MoveVerticalByLineBreak(direction);
		}
		FLOAT caretX = 0.0f;
		FLOAT caretY = 0.0f;
		DWRITE_HIT_TEST_METRICS caretMetrics{};
		if (FAILED(visual.layout->HitTestTextPosition(static_cast<UINT32>(DisplayCaret()), FALSE, &caretX, &caretY, &caretMetrics))) {
			return MoveVerticalByLineBreak(direction);
		}
		const float lineHeight = (std::max)(caretMetrics.height, detail::kLineHeight);
		const float targetX = desiredCaretX_.has_value() ? *desiredCaretX_ : caretX;
		desiredCaretX_ = targetX;
		const float targetY = direction < 0
			? (std::max)(0.0f, caretY - lineHeight * 0.5f)
			: caretY + lineHeight * 1.5f;
		BOOL trailing = FALSE;
		BOOL inside = FALSE;
		DWRITE_HIT_TEST_METRICS targetMetrics{};
		if (FAILED(visual.layout->HitTestPoint(targetX, targetY, &trailing, &inside, &targetMetrics))) {
			return direction < 0 ? 0 : text_.size();
		}
		const size_t index = targetMetrics.textPosition + (trailing ? targetMetrics.length : 0);
		return (std::min)(index, text_.size());
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
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F textColor_{};
	D2D1_COLOR_F mutedColor_{};
	D2D1_COLOR_F selectionColor_{};
	bool multiline_ = false;
	float scrollX_ = 0.0f;
	float scrollY_ = 0.0f;
	size_t caret_ = 0;
	size_t selectionAnchor_ = 0;
	bool draggingSelection_ = false;
	ScrollDragMode scrollDragMode_ = ScrollDragMode::None;
	D2D1_POINT_2F dragOrigin_{ 0.0f, 0.0f };
	float originScrollX_ = 0.0f;
	float originScrollY_ = 0.0f;
	mutable bool horizontalScrollHovered_ = false;
	mutable bool verticalScrollHovered_ = false;
	mutable std::chrono::steady_clock::time_point horizontalScrollRevealUntil_{};
	mutable std::chrono::steady_clock::time_point verticalScrollRevealUntil_{};
	mutable bool previousHorizontalOverflow_ = false;
	mutable bool previousVerticalOverflow_ = false;
	mutable float lastHorizontalMaxScroll_ = 0.0f;
	mutable float lastVerticalMaxScroll_ = 0.0f;
	mutable float horizontalHintMaxScroll_ = 0.0f;
	mutable float verticalHintMaxScroll_ = 0.0f;
	bool ensureCaretVisible_ = true;
	bool imeActive_ = false;
	bool caretTargetVisible_ = true;
	UIAnimation caretOpacityAnimation_{};
	UIAnimation horizontalScrollVisibilityAnimation_{};
	UIAnimation verticalScrollVisibilityAnimation_{};
	std::chrono::steady_clock::time_point nextCaretBlinkToggle_ = std::chrono::steady_clock::now();
	std::wstring imeComposition_;
	LONG imeCursorPos_ = 0;
	size_t compositionAnchor_ = 0;
	size_t compositionReplaceLength_ = 0;
	mutable std::optional<float> desiredCaretX_;
	std::function<void(std::wstring_view)> onChanged_;
	TextStyle textStyle_{};
	TextStyle labelStyle_{};
	bool styleOverride_ = false;
	bool labelStyleOverride_ = false;
	std::vector<StyledTextRange> styledRanges_;
	mutable TextLayoutCache labelLayoutCache_;
	mutable TextLayoutCache contentLayoutCache_;
};

class ListBox final : public UIComponent {
public:
	ListBox(std::vector<std::wstring> items,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& outlineColor,
		const D2D1_COLOR_F& textColor,
		std::function<void(std::wstring_view)> onChanged)
		: items_(std::move(items)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), accentColor_(accentColor), outlineColor_(outlineColor), textColor_(textColor), onChanged_(std::move(onChanged)), textStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		textStyleOverride_ = true;
	}

	void SetOnScrollChanged(std::function<void(float)> onScrollChanged) {
		onScrollChanged_ = std::move(onScrollChanged);
	}

	void SetScrollNormalized(float value) {
		const size_t maxOffset = items_.size() > VisibleRows() ? items_.size() - VisibleRows() : 0;
		topIndex_ = static_cast<size_t>(Clamp01(value) * static_cast<float>(maxOffset));
		NotifyScrollChanged();
	}

	float ScrollNormalized() const {
		const size_t maxOffset = items_.size() > VisibleRows() ? items_.size() - VisibleRows() : 0;
		if (maxOffset == 0) {
			return 0.0f;
		}
		return static_cast<float>(topIndex_) / static_cast<float>(maxOffset);
	}

	void Render(ID2D1DeviceContext* context) override {
		UpdateScrollBarAnimation(ShouldRevealScrollBar());
		context->FillRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		context->DrawRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.5f + 0.5f * FocusProgress()), 1.0f + FocusProgress());
		constexpr float kRowStride = 26.0f;
		const bool showScrollBar = NeedsScrollBar();
		const bool renderScrollBar = showScrollBar || ScrollBarOpacity() > 0.01f;
		D2D1_RECT_F content = InflateRect(bounds_, 10.0f);
		if (renderScrollBar) {
			content.right -= kScrollGutter;
		}
		context->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		for (size_t row = 0; row < VisibleRows(); ++row) {
			size_t index = topIndex_ + row;
			if (index >= items_.size()) {
				break;
			}
			const float rowTop = content.top + row * kRowStride;
			D2D1_RECT_F rowRect = D2D1::RectF(content.left + 2.0f, rowTop + 2.0f, content.right - 2.0f, rowTop + kRowStride - 2.0f);
			const auto metrics = MeasureTextMetrics(dwriteFactory_.Get(), format_.Get(), items_[index], textStyleOverride_ ? &textStyle_ : nullptr);
			const float textTop = rowRect.top + (std::max)(0.0f, std::floor(((rowRect.bottom - rowRect.top) - (std::max)(metrics.height, detail::kLineHeight)) * 0.5f));
			const D2D1_RECT_F textRect = D2D1::RectF(rowRect.left + 12.0f, textTop, rowRect.right - 12.0f, textTop + (std::max)(metrics.height, detail::kLineHeight) + 1.0f);
			if (index == selectedIndex_) {
				context->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_, 0.16f + 0.12f * FocusProgress()));
			}
			if (hoveredRow_ && *hoveredRow_ == index) {
				context->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_, 0.08f + 0.08f * HoverProgress()));
			}
			DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, textStyleOverride_ ? textStyle_.color : textColor_), items_[index], textRect, textStyleOverride_ ? &textStyle_ : nullptr);
		}
		context->PopAxisAlignedClip();
		if (renderScrollBar) {
			DrawRadixScrollbar(context, MakeScrollState(), PrepareSharedBrush(context, SharedBrushSlot::Primary, accentColor_), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_), ScrollBarOpacity());
		}
	}

	void OnPointerDown(D2D1_POINT_2F point) override {
		UIComponent::OnPointerDown(point);
		if (NeedsScrollBar() && MakeScrollState().HitThumb(point)) {
			draggingScroll_ = true;
			scrollBarHovered_ = true;
			TouchScrollReveal();
			dragOriginY_ = point.y;
			originTopIndex_ = topIndex_;
			return;
		}
		SelectRow(RowFromPoint(point));
	}

	void OnPointerUp(D2D1_POINT_2F point) override {
		draggingScroll_ = false;
		scrollBarHovered_ = NeedsScrollBar() && MakeScrollState().HitTrack(point);
		if (scrollBarHovered_) {
			TouchScrollReveal();
		}
		UIComponent::OnPointerUp(point);
	}

	void OnPointerMove(D2D1_POINT_2F point) override {
		if (draggingScroll_ && NeedsScrollBar()) {
			TouchScrollReveal();
			const auto state = MakeScrollState();
			const auto track = state.TrackBounds();
			const auto thumb = state.ThumbBounds();
			const float thumbHeight = thumb.bottom - thumb.top;
			const float trackHeight = (std::max)(1.0f, (track.bottom - track.top) - thumbHeight);
			const size_t maxOffset = items_.size() > VisibleRows() ? items_.size() - VisibleRows() : 0;
			if (maxOffset > 0) {
				const float deltaRatio = (point.y - dragOriginY_) / trackHeight;
				topIndex_ = (std::min)(maxOffset, static_cast<size_t>((std::max)(0.0f, std::round(originTopIndex_ + deltaRatio * maxOffset))));
				NotifyScrollChanged();
			}
			return;
		}
		hoveredRow_ = RowFromPoint(point);
	}

	void OnHover(bool hovered) override {
		UIComponent::OnHover(hovered);
		if (!hovered && !draggingScroll_) {
			scrollBarHovered_ = false;
		}
	}

	bool OnMouseWheel(int delta, D2D1_POINT_2F point) override {
		if (!PointInRect(bounds_, point) || !NeedsScrollBar()) {
			return false;
		}
		const size_t previous = topIndex_;
		if (delta > 0 && topIndex_ > 0) {
			--topIndex_;
		}
		else if (delta < 0 && topIndex_ + VisibleRows() < items_.size()) {
			++topIndex_;
		}
		NotifyScrollChanged();
		const bool changed = previous != topIndex_;
		if (changed) {
			TouchScrollReveal();
		}
		return changed;
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
	float ScrollBarOpacity() const {
		return AnimationSlotValue(L"scrollbarVisibility", 0.0f);
	}

	bool ScrollRevealActive() const {
		return scrollRevealUntil_ > std::chrono::steady_clock::now();
	}

	void TouchScrollReveal() const {
		scrollRevealUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
	}

	bool ShouldRevealScrollBar() const {
		return NeedsScrollBar() && (scrollBarHovered_ || draggingScroll_ || ScrollRevealActive());
	}

	void UpdateScrollBarAnimation(bool visible) {
		if (visible == scrollBarVisibleTarget_) {
			return;
		}
		scrollBarVisibleTarget_ = visible;
		AnimateSlotLinear(L"scrollbarVisibility", visible ? 1.0f : 0.0f, 0.16);
	}

	bool NeedsScrollBar() const {
		return items_.size() > VisibleRows();
	}

	size_t VisibleRows() const {
		return static_cast<size_t>((std::max)(1.0f, std::floor((bounds_.bottom - bounds_.top - 20.0f) / 26.0f)));
	}

	ScrollbarState MakeScrollState() const {
		ScrollbarState state{ ScrollOrientation::Vertical };
		state.viewport = bounds_;
		state.contentExtent = static_cast<float>(items_.size()) * 26.0f + 20.0f;
		state.offset = static_cast<float>(topIndex_) * 26.0f;
		state.inset = 10.0f;
		state.edgePadding = 2.0f;
		state.hovered = scrollBarHovered_ || draggingScroll_;
		state.dragging = draggingScroll_;
		return state;
	}

	CursorKind Cursor() const override { return CursorKind::Arrow; }

	CursorKind CursorAt(D2D1_POINT_2F point) const override {
		scrollBarHovered_ = NeedsScrollBar() && MakeScrollState().HitTrack(point);
		if (scrollBarHovered_) {
			TouchScrollReveal();
			return CursorKind::Arrow;
		}
		return CursorKind::Arrow;
	}

	std::optional<size_t> RowFromPoint(D2D1_POINT_2F point) const {
		if (!PointInRect(bounds_, point)) {
			return std::nullopt;
		}
		const float relativeY = point.y - bounds_.top - 10.0f;
		if (relativeY < 0.0f) {
			return std::nullopt;
		}
		size_t row = static_cast<size_t>(relativeY / 26.0f);
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

	void NotifyScrollChanged() {
		if (onScrollChanged_) {
			onScrollChanged_(ScrollNormalized());
		}
	}

	std::vector<std::wstring> items_;
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F textColor_{};
	TextStyle textStyle_{};
	bool textStyleOverride_ = false;
	size_t selectedIndex_ = 0;
	size_t topIndex_ = 0;
	std::optional<size_t> hoveredRow_;
	mutable bool scrollBarHovered_ = false;
	bool draggingScroll_ = false;
	bool scrollBarVisibleTarget_ = false;
	float dragOriginY_ = 0.0f;
	size_t originTopIndex_ = 0;
	mutable std::chrono::steady_clock::time_point scrollRevealUntil_{};
	std::function<void(std::wstring_view)> onChanged_;
	std::function<void(float)> onScrollChanged_;
	static constexpr float kScrollGutter = 18.0f;
};

class ChipStrip final : public UIComponent {
public:
	ChipStrip(std::wstring label,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		ComPtr<IDWriteTextFormat> captionFormat,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& outlineColor,
		const D2D1_COLOR_F& textColor,
		std::vector<std::wstring> items)
		: label_(std::move(label)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), captionFormat_(std::move(captionFormat)), surfaceColor_(surfaceColor), accentColor_(accentColor), outlineColor_(outlineColor), textColor_(textColor), items_(std::move(items)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }
	bool WantsFrameTick() const override { return ScrollRevealActive(); }

	void SetLabelStyle(const TextStyle& style) {
		labelStyle_ = style;
		labelStyleOverride_ = true;
	}

	void SetItemStyle(const TextStyle& style) {
		itemStyle_ = style;
		itemStyleOverride_ = true;
	}

	void OnAttachAnimations() override {
		RegisterAnimationSlot(L"scrollbarVisibility", 0.0f);
	}

	void Render(ID2D1DeviceContext* context) override {
		DrawStyledText(context, dwriteFactory_.Get(), captionFormat_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, labelStyleOverride_ ? labelStyle_.color : textColor_), label_, D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f), labelStyleOverride_ ? &labelStyle_ : nullptr);
		UpdateScrollBarAnimation(ShouldRevealScrollBar());
		const bool showScroll = NeedsScrollBar();
		const bool renderScroll = showScroll || ScrollBarOpacity() > 0.01f;
		D2D1_RECT_F viewport = D2D1::RectF(bounds_.left, bounds_.top + 22.0f, bounds_.right, bounds_.bottom);
		D2D1_RECT_F contentViewport = viewport;
		if (renderScroll) {
			contentViewport.bottom -= kScrollGutter;
		}
		context->FillRoundedRectangle(D2D1::RoundedRect(viewport, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		context->DrawRoundedRectangle(D2D1::RoundedRect(viewport, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.45f + 0.45f * FocusProgress()), 1.0f + FocusProgress());
		context->PushAxisAlignedClip(D2D1::RectF(contentViewport.left + 8.0f, contentViewport.top + 8.0f, contentViewport.right - 8.0f, contentViewport.bottom - 8.0f), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		float x = contentViewport.left + 12.0f - scrollOffset_;
		const float laneHeight = (std::max)(1.0f, contentViewport.bottom - contentViewport.top);
		for (size_t index = 0; index < items_.size(); ++index) {
			const auto metrics = MeasureTextMetrics(dwriteFactory_.Get(), format_.Get(), items_[index], itemStyleOverride_ ? &itemStyle_ : nullptr);
			const float pillWidth = (std::max)(68.0f, metrics.widthIncludingTrailingWhitespace + 28.0f);
			const float pillHeight = (std::min)(laneHeight - 18.0f, (std::max)(24.0f, metrics.height + 10.0f));
			const float pillTop = contentViewport.top + (std::max)(8.0f, std::floor((laneHeight - pillHeight) * 0.5f));
			const D2D1_RECT_F pill = D2D1::RectF(x, pillTop, x + pillWidth, pillTop + pillHeight);
			const float textTop = pill.top + (std::max)(0.0f, std::floor(((pill.bottom - pill.top) - (std::max)(metrics.height, detail::kLineHeight)) * 0.5f));
			const D2D1_RECT_F textRect = D2D1::RectF(pill.left + 14.0f, textTop, pill.right - 14.0f, textTop + (std::max)(metrics.height, detail::kLineHeight) + 1.0f);
			context->FillRoundedRectangle(D2D1::RoundedRect(pill, 999.0f, 999.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_, index == selectedIndex_ ? 0.18f : 0.08f));
			DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, itemStyleOverride_ ? itemStyle_.color : textColor_), items_[index], textRect, itemStyleOverride_ ? &itemStyle_ : nullptr);
			x += pillWidth + 12.0f;
		}
		context->PopAxisAlignedClip();
		if (renderScroll) {
			DrawRadixScrollbar(context, MakeScrollState(), PrepareSharedBrush(context, SharedBrushSlot::Primary, accentColor_), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_), ScrollBarOpacity());
		}
	}

	void OnPointerDown(D2D1_POINT_2F point) override {
		UIComponent::OnPointerDown(point);
		if (NeedsScrollBar() && MakeScrollState().HitThumb(point)) {
			draggingScroll_ = true;
			scrollBarHovered_ = true;
			TouchScrollReveal();
			dragOriginX_ = point.x;
			originScrollOffset_ = scrollOffset_;
			return;
		}
		const auto hit = HitChip(point);
		if (hit) {
			selectedIndex_ = *hit;
		}
	}

	void OnPointerMove(D2D1_POINT_2F point) override {
		if (draggingScroll_ && NeedsScrollBar()) {
			TouchScrollReveal();
			const auto state = MakeScrollState();
			const auto track = state.TrackBounds();
			const auto thumb = state.ThumbBounds();
			const float travel = (std::max)(1.0f, (track.right - track.left) - (thumb.right - thumb.left));
			scrollOffset_ = (std::clamp)(originScrollOffset_ + (point.x - dragOriginX_) * (MaxScrollOffset() / travel), 0.0f, MaxScrollOffset());
		}
	}

	void OnPointerUp(D2D1_POINT_2F point) override {
		draggingScroll_ = false;
		scrollBarHovered_ = NeedsScrollBar() && MakeScrollState().HitTrack(point);
		if (scrollBarHovered_) {
			TouchScrollReveal();
		}
		UIComponent::OnPointerUp(point);
	}

	void OnHover(bool hovered) override {
		UIComponent::OnHover(hovered);
		if (!hovered && !draggingScroll_) {
			scrollBarHovered_ = false;
		}
	}

	bool OnMouseWheel(int delta, D2D1_POINT_2F point) override {
		if (!PointInRect(bounds_, point) || !NeedsScrollBar()) {
			return false;
		}
		const float previous = scrollOffset_;
		scrollOffset_ = (std::clamp)(scrollOffset_ + (delta > 0 ? -26.0f : 26.0f), 0.0f, MaxScrollOffset());
		const bool changed = !NearlyEqual(previous, scrollOffset_, 0.05f);
		if (changed) {
			TouchScrollReveal();
		}
		return changed;
	}

	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_LEFT) scrollOffset_ = (std::max)(0.0f, scrollOffset_ - 24.0f);
		if (key == VK_RIGHT) scrollOffset_ = (std::min)(MaxScrollOffset(), scrollOffset_ + 24.0f);
	}

private:
	float ScrollBarOpacity() const {
		return AnimationSlotValue(L"scrollbarVisibility", 0.0f);
	}

	bool ScrollRevealActive() const {
		return scrollRevealUntil_ > std::chrono::steady_clock::now();
	}

	void TouchScrollReveal() const {
		scrollRevealUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
	}

	bool ShouldRevealScrollBar() const {
		return NeedsScrollBar() && (draggingScroll_ || scrollBarHovered_ || ScrollRevealActive());
	}

	void UpdateScrollBarAnimation(bool visible) {
		if (visible == scrollBarVisibleTarget_) {
			return;
		}
		scrollBarVisibleTarget_ = visible;
		AnimateSlotLinear(L"scrollbarVisibility", visible ? 1.0f : 0.0f, 0.16);
	}

	bool NeedsScrollBar() const {
		return MaxScrollOffset() > 1.0f;
	}

	float ContentWidth() const {
		float total = 12.0f;
		for (const auto& item : items_) {
			const auto metrics = MeasureTextMetrics(dwriteFactory_.Get(), format_.Get(), item, itemStyleOverride_ ? &itemStyle_ : nullptr);
			total += (std::max)(68.0f, metrics.widthIncludingTrailingWhitespace + 28.0f) + 12.0f;
		}
		return total;
	}

	float MaxScrollOffset() const {
		const float viewportWidth = (std::max)(1.0f, bounds_.right - bounds_.left - 16.0f);
		return (std::max)(0.0f, ContentWidth() - viewportWidth);
	}

	ScrollbarState MakeScrollState() const {
		ScrollbarState state{ ScrollOrientation::Horizontal };
		state.viewport = D2D1::RectF(bounds_.left, bounds_.top + 22.0f, bounds_.right, bounds_.bottom);
		state.contentExtent = (std::max)(1.0f, ContentWidth());
		state.offset = scrollOffset_;
		state.inset = 10.0f;
		state.edgePadding = 2.0f;
		state.hovered = draggingScroll_ || scrollBarHovered_;
		state.dragging = draggingScroll_;
		return state;
	}

	CursorKind CursorAt(D2D1_POINT_2F point) const override {
		scrollBarHovered_ = NeedsScrollBar() && MakeScrollState().HitTrack(point);
		if (scrollBarHovered_) {
			TouchScrollReveal();
			return CursorKind::Arrow;
		}
		return CursorKind::Hand;
	}

	std::optional<size_t> HitChip(D2D1_POINT_2F point) const {
		const D2D1_RECT_F viewport = D2D1::RectF(bounds_.left, bounds_.top + 22.0f, bounds_.right, bounds_.bottom - (NeedsScrollBar() ? kScrollGutter : 0.0f));
		const float laneHeight = (std::max)(1.0f, viewport.bottom - viewport.top);
		float x = viewport.left + 12.0f - scrollOffset_;
		for (size_t index = 0; index < items_.size(); ++index) {
			const auto metrics = MeasureTextMetrics(dwriteFactory_.Get(), format_.Get(), items_[index], itemStyleOverride_ ? &itemStyle_ : nullptr);
			const float pillWidth = (std::max)(68.0f, metrics.widthIncludingTrailingWhitespace + 28.0f);
			const float pillHeight = (std::min)(laneHeight - 18.0f, (std::max)(24.0f, metrics.height + 10.0f));
			const float pillTop = viewport.top + (std::max)(8.0f, std::floor((laneHeight - pillHeight) * 0.5f));
			const D2D1_RECT_F pill = D2D1::RectF(x, pillTop, x + pillWidth, pillTop + pillHeight);
			if (PointInRect(pill, point)) {
				return index;
			}
			x += pillWidth + 12.0f;
		}
		return std::nullopt;
	}

	std::wstring label_;
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	ComPtr<IDWriteTextFormat> captionFormat_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F textColor_{};
	TextStyle labelStyle_{};
	TextStyle itemStyle_{};
	bool labelStyleOverride_ = false;
	bool itemStyleOverride_ = false;
	std::vector<std::wstring> items_;
	float scrollOffset_ = 0.0f;
	bool draggingScroll_ = false;
	mutable bool scrollBarHovered_ = false;
	bool scrollBarVisibleTarget_ = false;
	float dragOriginX_ = 0.0f;
	float originScrollOffset_ = 0.0f;
	size_t selectedIndex_ = 0;
	mutable std::chrono::steady_clock::time_point scrollRevealUntil_{};
	static constexpr float kScrollGutter = 18.0f;
};

class ComboBox final : public UIComponent {
public:
	ComboBox(std::vector<std::wstring> items,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& outlineColor,
		const D2D1_COLOR_F& textColor,
		std::function<void(std::wstring_view)> onChanged)
		: items_(std::move(items)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), accentColor_(accentColor), outlineColor_(outlineColor), textColor_(textColor), onChanged_(std::move(onChanged)), textStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void SetTextStyle(const TextStyle& style) {
		textStyle_ = style;
		textStyleOverride_ = true;
	}

	bool HasOpenPopup() const {
		return open_ || openAnimation_.Value() > 0.01f;
	}

	bool HitTest(ID2D1Factory1*, D2D1_POINT_2F point) const override {
		return PointInRect(bounds_, point) || (open_ && PointInRect(PopupBounds(), point));
	}

	void Render(ID2D1DeviceContext* context) override {
		const float openAmount = openAnimation_.Value();
		context->FillRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		context->DrawRoundedRectangle(D2D1::RoundedRect(bounds_, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.5f + 0.5f * (std::max)(FocusProgress(), openAmount)), 1.0f + (std::max)(FocusProgress(), openAmount));
		if (!items_.empty()) {
			const std::wstring& selected = items_[selectedIndex_];
			const auto metrics = MeasureTextMetrics(dwriteFactory_.Get(), format_.Get(), selected, textStyleOverride_ ? &textStyle_ : nullptr);
			const float textTop = bounds_.top + (std::max)(0.0f, std::floor(((bounds_.bottom - bounds_.top) - (std::max)(metrics.height, detail::kLineHeight)) * 0.5f));
			DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, textStyleOverride_ ? textStyle_.color : textColor_), selected, D2D1::RectF(bounds_.left + 14.0f, textTop, bounds_.right - 30.0f, textTop + (std::max)(metrics.height, detail::kLineHeight) + 1.0f), textStyleOverride_ ? &textStyle_ : nullptr);
		}
		auto* outlineBrush = PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_);
		context->DrawLine(D2D1::Point2F(bounds_.right - 18.0f, bounds_.top + 14.0f), D2D1::Point2F(bounds_.right - 12.0f, bounds_.top + 22.0f), outlineBrush, 1.8f);
		context->DrawLine(D2D1::Point2F(bounds_.right - 12.0f, bounds_.top + 22.0f), D2D1::Point2F(bounds_.right - 6.0f, bounds_.top + 14.0f), outlineBrush, 1.8f);
		if (openAmount <= 0.01f) {
			return;
		}
		D2D1_RECT_F popup = PopupBounds(openAmount);
		context->FillRoundedRectangle(D2D1::RoundedRect(popup, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		context->DrawRoundedRectangle(D2D1::RoundedRect(popup, 12.0f, 12.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_), 1.5f);
		for (size_t index = 0; index < items_.size(); ++index) {
			D2D1_RECT_F rowRect = PopupRowRect(popup, index);
			const auto metrics = MeasureTextMetrics(dwriteFactory_.Get(), format_.Get(), items_[index], textStyleOverride_ ? &textStyle_ : nullptr);
			const float textTop = rowRect.top + (std::max)(0.0f, std::floor(((rowRect.bottom - rowRect.top) - (std::max)(metrics.height, detail::kLineHeight)) * 0.5f));
			const D2D1_RECT_F textRect = D2D1::RectF(rowRect.left + 12.0f, textTop, rowRect.right - 12.0f, textTop + (std::max)(metrics.height, detail::kLineHeight) + 1.0f);
			if (hoveredIndex_ && *hoveredIndex_ == index) {
				context->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_, 0.18f));
			}
			DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, textStyleOverride_ ? textStyle_.color : textColor_), items_[index], textRect, textStyleOverride_ ? &textStyle_ : nullptr);
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
		const float fullHeight = static_cast<float>(items_.size()) * PopupRowHeight() + 12.0f;
		return D2D1::RectF(bounds_.left, bounds_.bottom + 6.0f, bounds_.right, bounds_.bottom + 6.0f + fullHeight * openAmount);
	}

	float PopupRowHeight() const {
		return 28.0f;
	}

	D2D1_RECT_F PopupRowRect(const D2D1_RECT_F& popup, size_t index) const {
		const float rowTop = popup.top + 6.0f + static_cast<float>(index) * PopupRowHeight();
		return D2D1::RectF(popup.left + 6.0f, rowTop + 1.0f, popup.right - 6.0f, rowTop + PopupRowHeight() - 3.0f);
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
		size_t row = static_cast<size_t>(relativeY / PopupRowHeight());
		return row < items_.size() ? std::optional<size_t>(row) : std::nullopt;
	}

	std::vector<std::wstring> items_;
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F textColor_{};
	TextStyle textStyle_{};
	bool textStyleOverride_ = false;
	size_t selectedIndex_ = 0;
	bool open_ = false;
	UIAnimation openAnimation_{};
	std::optional<size_t> hoveredIndex_;
	std::function<void(std::wstring_view)> onChanged_;
};

class ScrollBar final : public UIComponent {
public:
	ScrollBar(ScrollOrientation orientation,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& outlineColor,
		float value,
		float pageSize,
		std::function<void(float)> onChanged)
		: orientation_(orientation), surfaceColor_(surfaceColor), accentColor_(accentColor), outlineColor_(outlineColor), value_(Clamp01(value)), pageSize_(Clamp01(pageSize)), onChanged_(std::move(onChanged)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }
	CursorKind Cursor() const override { return CursorKind::Arrow; }

	float Value() const {
		return value_;
	}

	void SetExternalValue(float value) {
		SetValue(value, false);
	}

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(valueAnimation_, value_);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		context->DrawRoundedRectangle(D2D1::RoundedRect(bounds_, 10.0f, 10.0f), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.5f + 0.5f * FocusProgress()), focused_ ? 1.4f : 1.0f);
		DrawRadixScrollbar(context, MakeScrollState(), PrepareSharedBrush(context, SharedBrushSlot::Primary, accentColor_), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_));
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); UpdateValue(point); }
	void OnPointerMove(D2D1_POINT_2F point) override { if (pressed_) UpdateValue(point); }
	void OnPointerUp(D2D1_POINT_2F point) override { if (pressed_) UpdateValue(point); UIComponent::OnPointerUp(point); }
	bool OnMouseWheel(int delta, D2D1_POINT_2F point) override {
		if (!PointInRect(bounds_, point)) {
			return false;
		}
		SetValue(value_ + (delta > 0 ? -0.05f : 0.05f));
		return true;
	}
	void OnKeyDown(WPARAM key, const KeyModifiers&) override {
		if (key == VK_LEFT || key == VK_UP) SetValue(value_ - 0.03f);
		if (key == VK_RIGHT || key == VK_DOWN) SetValue(value_ + 0.03f);
	}

private:
	ScrollbarState MakeScrollState() const {
		ScrollbarState state{ orientation_ };
		state.viewport = bounds_;
		const float viewportExtent = orientation_ == ScrollOrientation::Horizontal ? (bounds_.right - bounds_.left) : (bounds_.bottom - bounds_.top);
		state.contentExtent = viewportExtent / (std::max)(pageSize_, 0.15f);
		state.offset = valueAnimation_.Value() * (std::max)(0.0f, state.contentExtent - viewportExtent);
		state.inset = 2.0f;
		state.edgePadding = 2.0f;
		state.hovered = hovered_ || pressed_;
		state.dragging = pressed_;
		return state;
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

	void SetValue(float value, bool notify = true) {
		value_ = Clamp01(value);
		Animate(valueAnimation_, value_, 0.12);
		if (notify && onChanged_) onChanged_(value_);
	}

	ScrollOrientation orientation_ = ScrollOrientation::Horizontal;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F outlineColor_{};
	float value_ = 0.0f;
	float pageSize_ = 0.2f;
	UIAnimation valueAnimation_{};
	std::function<void(float)> onChanged_;
};

class Knob final : public UIComponent {
public:
	Knob(std::wstring label,
		ComPtr<IDWriteFactory> dwriteFactory,
		ComPtr<IDWriteTextFormat> format,
		const D2D1_COLOR_F& surfaceColor,
		const D2D1_COLOR_F& accentColor,
		const D2D1_COLOR_F& outlineColor,
		const D2D1_COLOR_F& textColor,
		float value,
		std::function<void(float)> onChanged)
		: label_(std::move(label)), dwriteFactory_(std::move(dwriteFactory)), format_(std::move(format)), surfaceColor_(surfaceColor), accentColor_(accentColor), outlineColor_(outlineColor), textColor_(textColor), value_(Clamp01(value)), onChanged_(std::move(onChanged)), labelStyle_(CaptureTextStyle(format_.Get(), textColor_)), valueStyle_(CaptureTextStyle(format_.Get(), textColor_)) {}

	bool IsDynamic() const override { return true; }
	bool IsFocusable() const override { return true; }

	void SetLabelStyle(const TextStyle& style) {
		labelStyle_ = style;
		labelStyleOverride_ = true;
	}

	void SetValueStyle(const TextStyle& style) {
		valueStyle_ = style;
		valueStyleOverride_ = true;
	}

	void SetFullCircle(bool fullCircle) {
		fullCircle_ = fullCircle;
	}

	void OnAttachAnimations() override {
		if (animator_) {
			animator_->Attach(valueAnimation_, value_);
		}
	}

	void Render(ID2D1DeviceContext* context) override {
		DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, labelStyleOverride_ ? labelStyle_.color : textColor_), label_, D2D1::RectF(bounds_.left, bounds_.top, bounds_.right, bounds_.top + 18.0f), labelStyleOverride_ ? &labelStyle_ : nullptr);
		D2D1_POINT_2F center{ (bounds_.left + bounds_.right) * 0.5f, bounds_.top + 70.0f };
		const float radius = 36.0f;
		context->FillEllipse(D2D1::Ellipse(center, radius, radius), PrepareSharedBrush(context, SharedBrushSlot::Primary, surfaceColor_));
		context->DrawEllipse(D2D1::Ellipse(center, radius, radius), PrepareSharedBrush(context, SharedBrushSlot::Secondary, outlineColor_, 0.6f + 0.4f * FocusProgress()), 1.0f + FocusProgress());
		const float value = valueAnimation_.Value();
		constexpr float kPi = 3.1415926f;
		constexpr float kTau = 6.2831853f;
		const float angle = fullCircle_ ? (-kPi * 0.5f + value * kTau) : (kPi * (1.25f + value * 1.5f));
		D2D1_POINT_2F handle{ center.x + std::cos(angle) * 26.0f, center.y + std::sin(angle) * 26.0f };
		context->DrawLine(center, handle, PrepareSharedBrush(context, SharedBrushSlot::Secondary, accentColor_, 0.7f + 0.15f * HoverProgress() + 0.15f * PressProgress()), 3.0f);
		std::wstringstream ss;
		ss << static_cast<int>(value * 100.0f);
		auto valueText = ss.str();
		DrawStyledText(context, dwriteFactory_.Get(), format_.Get(), PrepareSharedBrush(context, SharedBrushSlot::Tertiary, valueStyleOverride_ ? valueStyle_.color : textColor_), valueText, D2D1::RectF(bounds_.left, center.y + 42.0f, bounds_.right, bounds_.bottom), valueStyleOverride_ ? &valueStyle_ : nullptr);
	}

	void OnPointerDown(D2D1_POINT_2F point) override { UIComponent::OnPointerDown(point); anchor_ = point; anchorValue_ = value_; }
	void OnPointerMove(D2D1_POINT_2F point) override {
		if (!pressed_) return;
		const float delta = (anchor_.x - point.x + point.y - anchor_.y) * 0.005f;
		SetValue(anchorValue_ + delta);
	}
	void OnPointerUp(D2D1_POINT_2F point) override { UIComponent::OnPointerUp(point); }
	bool OnMouseWheel(int delta, D2D1_POINT_2F point) override {
		if (!PointInRect(bounds_, point)) {
			return false;
		}
		SetValue(value_ + (delta > 0 ? 0.04f : -0.04f));
		return true;
	}
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
	ComPtr<IDWriteFactory> dwriteFactory_;
	ComPtr<IDWriteTextFormat> format_;
	D2D1_COLOR_F surfaceColor_{};
	D2D1_COLOR_F accentColor_{};
	D2D1_COLOR_F outlineColor_{};
	D2D1_COLOR_F textColor_{};
	TextStyle labelStyle_{};
	TextStyle valueStyle_{};
	bool labelStyleOverride_ = false;
	bool valueStyleOverride_ = false;
	bool fullCircle_ = false;
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

	bool NeedsContinuousRedraw() const {
		return focused_ != nullptr || captured_ != nullptr || dynamicDirty_ || animationSystem_.HasRunningAnimations() || HasPendingFrameTicks();
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
				UpdateCardHoverState(point, nullptr);
				ClearHover();
				currentCursor_ = CursorKind::None;
				dynamicDirty_ = true;
				return;
			}
			UIComponent* target = captured_ ? captured_ : ResolvePointerTarget(point, true);
			UpdateCardHoverState(point, target);
			SetHovered(target == leftCard_ || target == rightCard_ ? nullptr : target);
			if (CardScrollTarget(point)) {
				currentCursor_ = CursorKind::Arrow;
			}
			else {
				currentCursor_ = (target && target != leftCard_ && target != rightCard_) ? target->CursorAt(point) : CursorKind::Arrow;
			}
			if (captured_) {
				captured_->OnPointerMove(point);
				if (captured_ == leftCard_ || captured_ == rightCard_) {
					layoutDirty_ = true;
				}
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
			if (auto* cardScrollTarget = CardScrollTarget(point)) {
				SetFocused(nullptr);
				captured_ = cardScrollTarget;
				UpdateCardHoverState(point, cardScrollTarget);
				currentCursor_ = CursorKind::Arrow;
				cardScrollTarget->OnPointerDown(point);
				layoutDirty_ = true;
				dynamicDirty_ = true;
				return;
			}
			UIComponent* target = ResolvePointerTarget(point, true);
			UpdateCardHoverState(point, target);
			currentCursor_ = (target && target != leftCard_ && target != rightCard_) ? target->CursorAt(point) : CursorKind::Arrow;
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
				if (captured_ == leftCard_ || captured_ == rightCard_) {
					layoutDirty_ = true;
				}
				captured_ = nullptr;
				UIComponent* target = PointInViewport(point) ? ResolvePointerTarget(point, true) : nullptr;
				UpdateCardHoverState(point, target);
				SetHovered(target == leftCard_ || target == rightCard_ ? nullptr : target);
				if (PointInViewport(point) && CardScrollTarget(point)) {
					currentCursor_ = CursorKind::Arrow;
				}
				else {
					currentCursor_ = (target && target != leftCard_ && target != rightCard_) ? target->CursorAt(point) : (PointInViewport(point) ? CursorKind::Arrow : CursorKind::None);
				}
				dynamicDirty_ = true;
			}
			return;
		}
		case WM_MOUSELEAVE: {
			UpdateCardHoverState(point, nullptr);
			ClearHover();
			currentCursor_ = CursorKind::None;
			dynamicDirty_ = true;
			return;
		}
		case WM_MOUSEWHEEL: {
			const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			if (auto* modal = ResolveModalComponent(point, false)) {
				if (modal->OnMouseWheel(delta, point)) {
					dynamicDirty_ = true;
					return;
				}
			}

			auto candidates = CollectCandidates(point);
			std::vector<UIComponent*> ordered;
			ordered.reserve(candidates.size());
			for (const auto* candidate : candidates) {
				ordered.push_back(const_cast<UIComponent*>(candidate));
			}
			std::ranges::sort(ordered, [](const UIComponent* lhs, const UIComponent* rhs) {
				return lhs->ZIndex() > rhs->ZIndex();
			});

			for (auto* target : ordered) {
				if (target && target->OnMouseWheel(delta, point)) {
					dynamicDirty_ = true;
					return;
				}
			}

			if (auto* card = CardAtPoint(point)) {
				if (card->OnMouseWheel(delta, point)) {
					layoutDirty_ = true;
					dynamicDirty_ = true;
				}
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
			std::vector<UIComponent*> focusable;
			for (auto* component : focusOrder_) {
				if (component && component->CanFocus()) {
					focusable.push_back(component);
				}
			}
			if (focusable.empty()) {
				return false;
			}
			auto it = std::ranges::find(focusable, focused_);
			if (it == focusable.end()) {
				SetFocused(focusable.front());
			}
			else {
				++it;
				if (it == focusable.end()) {
					it = focusable.begin();
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
			RenderCard(targetContext, rightCard_, rightLayoutOrder_);
		}
		if (leftCard_ && leftCard_->Visible()) {
			RenderCard(targetContext, leftCard_, leftLayoutOrder_);
		}
		if (comboBox_ && comboBox_->Visible() && comboBox_->HasOpenPopup()) {
			comboBox_->Render(targetContext);
		}
		dynamicDirty_ = false;
	}

private:
	using TextBlock = detail::TextBlock;
	using ExpandableNote = detail::ExpandableNote;
	using ScrollArea = detail::ScrollArea;
	using CardSurface = detail::ScrollArea;
	using Button = detail::Button;
	using ImageFrame = detail::ImageFrame;
	using ImageButton = detail::ImageButton;
	using Checkbox = detail::Checkbox;
	using RadioButton = detail::RadioButton;
	using Slider = detail::Slider;
	using ProgressBar = detail::ProgressBar;
	using TextInput = detail::TextInput;
	using ListBox = detail::ListBox;
	using ChipStrip = detail::ChipStrip;
	using ComboBox = detail::ComboBox;
	using ScrollBar = detail::ScrollBar;
	using Knob = detail::Knob;
	using ScrollOrientation = detail::ScrollOrientation;

	UIComponent* ResolveModalComponent(D2D1_POINT_2F point, bool captureOutside) const {
		if (comboBox_ && comboBox_->Visible() && comboBox_->HasOpenPopup()) {
			if (captureOutside || comboBox_->HitTest(graphics_.d2dFactory.Get(), point)) {
				return comboBox_;
			}
		}
		return nullptr;
	}

	UIComponent* ResolvePointerTarget(D2D1_POINT_2F point, bool captureOutsideModal) const {
		if (auto* modal = ResolveModalComponent(point, captureOutsideModal)) {
			return modal;
		}
		return Pick(point);
	}

	CardSurface* CardScrollTarget(D2D1_POINT_2F point) const {
		if (rightCard_ && rightCard_->Visible() && rightCard_->HitScrollBar(point)) {
			return rightCard_;
		}
		if (leftCard_ && leftCard_->Visible() && leftCard_->HitScrollBar(point)) {
			return leftCard_;
		}
		return nullptr;
	}

	CardSurface* CardAtPoint(D2D1_POINT_2F point) const {
		if (rightCard_ && rightCard_->Visible() && UIComponent::PointInRect(rightCard_->Bounds(), point)) {
			return rightCard_;
		}
		if (leftCard_ && leftCard_->Visible() && UIComponent::PointInRect(leftCard_->Bounds(), point)) {
			return leftCard_;
		}
		return nullptr;
	}

	void UpdateCardHoverState(D2D1_POINT_2F point, UIComponent* target) {
		auto updateCard = [&](CardSurface* card) {
			if (!card || !card->Visible()) {
				return;
			}
			if (captured_ == card) {
				return;
			}
			if (target == card || UIComponent::PointInRect(card->Bounds(), point)) {
				card->CursorAt(point);
			}
			else {
				card->OnHover(false);
			}
		};
		updateCard(leftCard_);
		updateCard(rightCard_);
	}

	void RenderCard(ID2D1DeviceContext* context, CardSurface* card, const std::vector<UIComponent*>& items) {
		if (!context || !card || !card->Visible()) {
			return;
		}
		card->Render(context);
		const auto clip = card->ContentClipBounds();
		context->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		for (auto* item : items) {
			if (!item || !item->Visible()) {
				continue;
			}
			if (item == comboBox_ && comboBox_ && comboBox_->HasOpenPopup()) {
				continue;
			}
			item->Render(context);
		}
		context->PopAxisAlignedClip();
	}

	void ArrangeCard(CardSurface* card, std::span<UIComponent*> items) {
		if (!card) {
			return;
		}
		float contentHeight = layout_->Arrange(items, card->LayoutBounds(false), dpiScale_, card->ScrollOffset());
		card->SetContentMetrics(contentHeight);
		const bool needsScrollBar = card->ShouldReserveVerticalScrollBar();
		const float arrangedOffset = card->ScrollOffset();
		contentHeight = layout_->Arrange(items, card->LayoutBounds(needsScrollBar), dpiScale_, arrangedOffset);
		card->SetContentMetrics(contentHeight);
		if (card->ScrollOffset() != arrangedOffset) {
			layout_->Arrange(items, card->LayoutBounds(card->ShouldReserveVerticalScrollBar()), dpiScale_, card->ScrollOffset());
		}
	}

	void InitializeResources() {
		if (!graphics_.d2dContext || !graphics_.dwriteFactory) {
			return;
		}
		detail::EnsureSharedBrushPool(graphics_.d2dContext.Get());

		graphics_.dwriteFactory->CreateTextFormat(L"Segoe UI Semibold", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &titleFormat_);
		graphics_.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &buttonFormat_);
		graphics_.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &bodyFormat_);
		graphics_.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &fieldFormat_);
		graphics_.dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &captionFormat_);
		if (titleFormat_) titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
		if (buttonFormat_) buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		if (bodyFormat_) bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		if (fieldFormat_) fieldFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
		if (captionFormat_) captionFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
	}

	void BuildDemoScene() {
		layout_ = std::make_unique<VerticalStackLayout>(24.0f, 8.0f);
		components_.clear();
		leftLayoutOrder_.clear();
		rightLayoutOrder_.clear();
		focusOrder_.clear();

		const auto panelColor = detail::MakeColor(0.988f, 0.988f, 0.992f, 0.98f);
		const auto outlineColor = detail::MakeColor(0.89f, 0.89f, 0.91f, 1.0f);
		const auto accentColor = detail::MakeColor(0.106f, 0.165f, 0.325f, 1.0f);
		const auto accentSoftColor = detail::MakeColor(0.106f, 0.165f, 0.325f, 0.14f);
		const auto textColor = detail::MakeColor(0.09f, 0.09f, 0.11f, 1.0f);
		const auto mutedTextColor = detail::MakeColor(0.45f, 0.45f, 0.50f, 1.0f);
		const auto surfaceColor = detail::MakeColor(1.0f, 1.0f, 1.0f, 1.0f);
		const auto surfaceAltColor = detail::MakeColor(0.964f, 0.965f, 0.973f, 1.0f);
		const auto primaryColor = detail::MakeColor(0.09f, 0.09f, 0.11f, 1.0f);

		auto selectedAimStyle = std::make_shared<int>(0);

		auto leftCard = std::make_unique<CardSurface>(panelColor, outlineColor, accentColor, accentSoftColor);
		leftCard->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth, 540.0f));
		leftCard->SetZIndex(0);
		leftCard_ = leftCard.get();
		components_.push_back(std::move(leftCard));

		auto rightCard = std::make_unique<CardSurface>(panelColor, outlineColor, accentColor, accentSoftColor);
		rightCard->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth, 540.0f));
		rightCard->SetZIndex(0);
		rightCard_ = rightCard.get();
		components_.push_back(std::move(rightCard));

		auto title = std::make_unique<TextBlock>(L"Command Surface", graphics_.dwriteFactory, titleFormat_, textColor);
		title->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 34.0f));
		title->SetZIndex(1);
		leftLayoutOrder_.push_back(title.get());
		components_.push_back(std::move(title));

		auto subtitle = std::make_unique<TextBlock>(L"Radix / shadcn inspired tokens, focus rings, clipped surfaces and composite widgets.", graphics_.dwriteFactory, captionFormat_, mutedTextColor);
		subtitle->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 32.0f));
		subtitle->SetZIndex(1);
		subtitle->SetStyledRanges({
			StyledTextRange{ 0, 16, TextStyle{ .fontFamily = L"Segoe UI", .fontSize = 12.0f, .weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, .style = DWRITE_FONT_STYLE_NORMAL, .stretch = DWRITE_FONT_STRETCH_NORMAL, .color = detail::MakeColor(0.45f, 0.45f, 0.50f, 1.0f), .underline = false, .strikethrough = false, .horizontalAlign = HorizontalAlign::Left, .verticalAlign = VerticalAlign::Top, .locale = L"en-us" } },
			StyledTextRange{ 17, 7, TextStyle{ .fontFamily = L"Segoe UI", .fontSize = 12.0f, .weight = DWRITE_FONT_WEIGHT_NORMAL, .style = DWRITE_FONT_STYLE_ITALIC, .stretch = DWRITE_FONT_STRETCH_NORMAL, .color = detail::MakeColor(0.45f, 0.45f, 0.50f, 1.0f), .underline = true, .strikethrough = false, .horizontalAlign = HorizontalAlign::Left, .verticalAlign = VerticalAlign::Top, .locale = L"en-us" } }
		});
		leftLayoutOrder_.push_back(subtitle.get());
		components_.push_back(std::move(subtitle));

		auto preview = std::make_unique<ImageFrame>(outlineColor, accentColor, surfaceAltColor);
		preview->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 110.0f));
		preview->SetZIndex(1);
		preview_ = preview.get();
		leftLayoutOrder_.push_back(preview.get());
		components_.push_back(std::move(preview));

		auto previewButton = std::make_unique<ImageButton>(L"Open Preview", graphics_.dwriteFactory, buttonFormat_, surfaceColor, accentColor, outlineColor, textColor, [this]() {
			SetStatus(L"Image button activated from the header-only UI host.");
		});
		previewButton->SetTextAlignment(HorizontalAlign::Left, VerticalAlign::Center);
		previewButton->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 46.0f));
		previewButton->SetZIndex(3);
		focusOrder_.push_back(previewButton.get());
		leftLayoutOrder_.push_back(previewButton.get());
		components_.push_back(std::move(previewButton));

		auto resetButton = std::make_unique<Button>(L"Reset Canvas", graphics_.dwriteFactory, buttonFormat_, primaryColor, surfaceAltColor, outlineColor, surfaceColor, [this]() {
			if (callbacks_.onResetCanvas) {
				callbacks_.onResetCanvas();
			}
			SetStatus(L"Reset requested from the native UI layer.");
		}, true);
		resetButton->SetTextAlignment(HorizontalAlign::Center, VerticalAlign::Center);
		resetButton->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		resetButton->SetZIndex(3);
		focusOrder_.push_back(resetButton.get());
		leftLayoutOrder_.push_back(resetButton.get());
		components_.push_back(std::move(resetButton));

		auto checkbox = std::make_unique<Checkbox>(L"Mirror show-grid state", graphics_.dwriteFactory, bodyFormat_, surfaceColor, accentColor, textColor, outlineColor, true, [this](bool checked) {
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

		auto radio0 = std::make_unique<RadioButton>(L"Aim style 0: ring", graphics_.dwriteFactory, bodyFormat_, surfaceColor, accentColor, textColor, outlineColor, selectedAimStyle, 0, [this](int value) {
			if (callbacks_.onAimStyleChanged) callbacks_.onAimStyleChanged(value);
			SetStatus(L"Aim style switched to ring mode.");
		});
		radio0->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		radio0->SetZIndex(3);
		focusOrder_.push_back(radio0.get());
		leftLayoutOrder_.push_back(radio0.get());
		components_.push_back(std::move(radio0));

		auto radio1 = std::make_unique<RadioButton>(L"Aim style 1: dot", graphics_.dwriteFactory, bodyFormat_, surfaceColor, accentColor, textColor, outlineColor, selectedAimStyle, 1, [this](int value) {
			if (callbacks_.onAimStyleChanged) callbacks_.onAimStyleChanged(value);
			SetStatus(L"Aim style switched to dot mode.");
		});
		radio1->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		radio1->SetZIndex(3);
		focusOrder_.push_back(radio1.get());
		leftLayoutOrder_.push_back(radio1.get());
		components_.push_back(std::move(radio1));

		auto radio2 = std::make_unique<RadioButton>(L"Aim style 2: triangle", graphics_.dwriteFactory, bodyFormat_, surfaceColor, accentColor, textColor, outlineColor, selectedAimStyle, 2, [this](int value) {
			if (callbacks_.onAimStyleChanged) callbacks_.onAimStyleChanged(value);
			SetStatus(L"Aim style switched to triangle mode.");
		});
		radio2->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, detail::kControlHeight));
		radio2->SetZIndex(3);
		focusOrder_.push_back(radio2.get());
		leftLayoutOrder_.push_back(radio2.get());
		components_.push_back(std::move(radio2));

		auto slider = std::make_unique<Slider>(L"Aim radius", graphics_.dwriteFactory, captionFormat_, surfaceAltColor, accentColor, textColor, outlineColor, 0.2f, [this](float value) {
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

		auto progress = std::make_unique<ProgressBar>(L"UIAnimation bridge", graphics_.dwriteFactory, captionFormat_, surfaceAltColor, accentColor, textColor, &progressAnimation_);
		progress->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 40.0f));
		progress->SetZIndex(3);
		leftLayoutOrder_.push_back(progress.get());
		components_.push_back(std::move(progress));

		auto singleInput = std::make_unique<TextInput>(L"Single-line input", L"Type a command...", L"draw hitmarker", graphics_.dwriteFactory, fieldFormat_, surfaceColor, outlineColor, textColor, mutedTextColor, accentColor, false, [this](std::wstring_view value) {
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

		auto multiInput = std::make_unique<TextInput>(L"Multiline editor", L"Notes...", L"Header-only migration complete.\nNext: richer text selection and IME support.", graphics_.dwriteFactory, fieldFormat_, surfaceColor, outlineColor, textColor, mutedTextColor, accentColor, true, [this](std::wstring_view) {
			SetStatus(L"Multiline editor changed.");
		});
		multiInput->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 118.0f));
		multiInput->SetZIndex(3);
		multiInput_ = multiInput.get();
		focusOrder_.push_back(multiInput_);
		rightLayoutOrder_.push_back(multiInput_);
		components_.push_back(std::move(multiInput));

		auto listBox = std::make_unique<ListBox>(std::vector<std::wstring>{ L"Aster", L"Beryl", L"Cinder", L"Delta", L"Ember", L"Flint", L"Grove", L"Halo", L"Iris", L"Juniper", L"Kite", L"Lumen" }, graphics_.dwriteFactory, bodyFormat_, surfaceColor, accentColor, outlineColor, textColor, [this](std::wstring_view value) {
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

		auto comboBox = std::make_unique<ComboBox>(std::vector<std::wstring>{ L"Telemetry", L"Diagnostics", L"Staging", L"Release" }, graphics_.dwriteFactory, bodyFormat_, surfaceColor, accentColor, outlineColor, textColor, [this](std::wstring_view value) {
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

		auto chipStrip = std::make_unique<ChipStrip>(L"Horizontal overflow", graphics_.dwriteFactory, bodyFormat_, captionFormat_, surfaceAltColor, accentColor, outlineColor, textColor, std::vector<std::wstring>{ L"Telemetry", L"Render Thread", L"Composition", L"Clipboard", L"IME", L"Selection", L"VirtualSurface", L"Animation" });
		chipStrip->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 76.0f));
		chipStrip->SetZIndex(3);
		focusOrder_.push_back(chipStrip.get());
		rightLayoutOrder_.push_back(chipStrip.get());
		components_.push_back(std::move(chipStrip));

		auto knob = std::make_unique<Knob>(L"Encoder", graphics_.dwriteFactory, captionFormat_, surfaceAltColor, accentColor, outlineColor, textColor, 0.42f, [this](float value) {
			std::wstringstream ss;
			ss << L"Knob rotated to " << static_cast<int>(value * 100.0f) << L"%.";
			SetStatus(ss.str());
		});
		knob->SetFullCircle(true);
		knob->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 136.0f));
		knob->SetZIndex(3);
		knob_ = knob.get();
		focusOrder_.push_back(knob_);
		rightLayoutOrder_.push_back(knob_);
		components_.push_back(std::move(knob));

		auto statusText = std::make_unique<ExpandableNote>(L"Animation extension demo", L"This block uses the generic animation-slot API on UIComponent. Click to expand or collapse and reuse the same slot pattern in custom components.", graphics_.dwriteFactory, bodyFormat_, captionFormat_, surfaceAltColor, outlineColor, textColor, mutedTextColor);
		statusText->SetBounds(D2D1::RectF(0.0f, 0.0f, detail::kCardWidth - 48.0f, 84.0f));
		statusText->SetZIndex(3);
		statusText_ = statusText.get();
		focusOrder_.push_back(statusText_);
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
		const bool twoColumn = availableWidth >= 420.0f;
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
			const float cardWidth = (std::min)(detail::kCardWidth, (availableWidth - gap) * 0.5f);
			const D2D1_RECT_F leftCardBounds = D2D1::RectF(viewport_.left + 24.0f, top, viewport_.left + 24.0f + cardWidth, bottom);
			const D2D1_RECT_F rightCardBounds = D2D1::RectF(leftCardBounds.right + gap, top, leftCardBounds.right + gap + cardWidth, bottom);
			leftCard_->SetBounds(leftCardBounds);
			rightCard_->SetBounds(rightCardBounds);
			ArrangeCard(leftCard_, leftLayoutOrder_);
			ArrangeCard(rightCard_, rightLayoutOrder_);
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
			ArrangeCard(rightCard_, rightLayoutOrder_);
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
		if (comboBox_ && comboBox_->Visible() && comboBox_->HasOpenPopup() && comboBox_->HitTest(graphics_.d2dFactory.Get(), point)) {
			return comboBox_;
		}
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

	bool HasPendingFrameTicks() const {
		for (const auto& component : components_) {
			if (component && component->Visible() && component->WantsFrameTick()) {
				return true;
			}
		}
		return false;
	}

	void SetStatus(std::wstring text) {
		if (statusText_) {
			statusText_->SetBody(std::move(text));
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
	ComPtr<IDWriteTextFormat> titleFormat_;
	ComPtr<IDWriteTextFormat> buttonFormat_;
	ComPtr<IDWriteTextFormat> bodyFormat_;
	ComPtr<IDWriteTextFormat> fieldFormat_;
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
	ImageFrame* preview_ = nullptr;
	ExpandableNote* statusText_ = nullptr;
	TextInput* singleInput_ = nullptr;
	TextInput* multiInput_ = nullptr;
	ListBox* listBox_ = nullptr;
	ComboBox* comboBox_ = nullptr;
	Knob* knob_ = nullptr;
};

}