#include "input/MouseState.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "WindowSystem.h"

#include <algorithm>
#include <cmath>

using namespace MouseInputs;

namespace
{
	// a click is reported for at least this long, so a game that reads at 60 Hz sees it
	constexpr auto kMinClick = std::chrono::milliseconds(50);
	// one wheel notch is "pressed" this long
	constexpr auto kWheelPulse = std::chrono::milliseconds(100);
	// relative motion stops when the mouse hasn't moved for this long
	constexpr auto kMoveTimeout = std::chrono::milliseconds(60);
}

MouseState& MouseState::Get()
{
	static MouseState s_instance;
	return s_instance;
}

void MouseState::on_move(Source source, int x, int y)
{
	if (source >= kSourceCount)
		return;
	std::scoped_lock lock(m_mutex);
	auto& w = m_windows[source];
	const auto now = Clock::now();
	const float dt = std::chrono::duration<float>(now - w.last_move).count();
	if (w.has_position && dt > 0.0005f && dt < 0.1f)
	{
		// smoothed speed (events come at the display rate or faster)
		const float vx = (float)(x - w.x) / dt, vy = (float)(y - w.y) / dt;
		w.vel_x = w.vel_x * 0.5f + vx * 0.5f;
		w.vel_y = w.vel_y * 0.5f + vy * 0.5f;
	}
	else
		w.vel_x = w.vel_y = 0;
	w.x = x;
	w.y = y;
	w.has_position = true;
	w.last_move = now;
	if (source != kSettings)
		m_last_window = source;
}

void MouseState::on_button(Source source, int button, bool down, int x, int y)
{
	if (source >= kSourceCount || button < 0 || button >= (int)kButtonCount)
		return;
	{
		std::scoped_lock lock(m_mutex);
		auto& w = m_windows[source];
		const auto now = Clock::now();
		if (down && !w.down[button])
			w.pressed_at[button] = now;
		else if (!down && w.down[button])
			w.hold_until[button] = w.pressed_at[button] + kMinClick;
		w.down[button] = down;
		w.x = x;
		w.y = y;
		w.has_position = true;
		if (source != kSettings)
			m_last_window = source;
	}
}

void MouseState::on_wheel(Source source, float notches)
{
	if (source >= kSourceCount || notches == 0)
		return;
	std::scoped_lock lock(m_mutex);
	auto& w = m_windows[source];
	const auto now = Clock::now();
	// each notch adds a pulse (a fast spin turns further)
	const int count = std::max(1, (int)std::lround(std::abs(notches)));
	auto& until = notches > 0 ? w.wheel_up_until : w.wheel_down_until;
	until = std::max(until, now) + kWheelPulse * count;
	// don't let a long spin run on for seconds
	until = std::min(until, now + kWheelPulse * 5);
	if (source != kSettings)
		m_last_window = source;
}

void MouseState::set_settings_buttons(uint32_t mask, bool active)
{
	std::scoped_lock lock(m_mutex);
	m_settings_active = active;
	auto& w = m_windows[kSettings];
	const auto now = Clock::now();
	for (size_t b = 0; b < kButtonCount; ++b)
	{
		const bool down = active && (mask & (1u << b)) != 0;
		if (down && !w.down[b])
			w.pressed_at[b] = now;
		else if (!down && w.down[b])
			w.hold_until[b] = w.pressed_at[b] + kMinClick;
		w.down[b] = down;
	}
	if (!active)
		w.wheel_up_until = w.wheel_down_until = {};
}

bool MouseState::button_active(const Window& w, size_t button, Clock::time_point now) const
{
	return w.down[button] || now < w.hold_until[button];
}

bool MouseState::button_down(Source source, int button) const
{
	if (source >= kSourceCount || button < 0 || button >= (int)kButtonCount)
		return false;
	std::scoped_lock lock(m_mutex);
	return button_active(m_windows[source], button, Clock::now());
}

int MouseState::window_source(MouseInputs::Window window) const
{
	switch (window)
	{
	case kTV:
		return kMainWindow;
	case kGamePad:
		return WindowSystem::IsPadWindowOpen() ? kPadWindow : kMainWindow;
	default:
		return m_last_window;
	}
}

bool MouseState::cursor(int source, float& x, float& y) const
{
	const auto& w = m_windows[source];
	x = y = 0;
	if (!w.has_position)
		return false;
	int ix, iy, iw, ih;
	LatteRenderTarget_getScreenImageArea(&ix, &iy, &iw, &ih, nullptr, nullptr, source == kPadWindow);
	if (iw <= 0 || ih <= 0)
		return false;
	x = std::clamp((float)(w.x - ix) / (float)iw, 0.0f, 1.0f) * 2.0f - 1.0f;
	y = std::clamp((float)(w.y - iy) / (float)ih, 0.0f, 1.0f) * 2.0f - 1.0f;
	return true;
}

bool MouseState::pressed(uint64_t id) const
{
	if (!IsPressable(id))
		return false;
	return value(id) > 0.5f;
}

float MouseState::value(uint64_t id) const
{
	if (!IsValid(id))
		return 0;
	const auto window = WindowOf(id);
	const auto input = InputOf(id);
	const auto now = Clock::now();
	std::scoped_lock lock(m_mutex);

	// buttons and the wheel: in the window, or (no prefix) in any game window or the settings window
	std::array<int, 3> sources{};
	size_t source_count = 0;
	if (window == kAny)
	{
		sources[source_count++] = kMainWindow;
		sources[source_count++] = kPadWindow;
		if (m_settings_active)
			sources[source_count++] = kSettings;
	}
	else
		sources[source_count++] = window_source(window);

	if (input < kButtonCount)
	{
		for (size_t i = 0; i < source_count; ++i)
		{
			if (button_active(m_windows[sources[i]], input, now))
				return 1.0f;
		}
		return 0;
	}
	if (input == kWheelUp || input == kWheelDown)
	{
		for (size_t i = 0; i < source_count; ++i)
		{
			const auto& w = m_windows[sources[i]];
			if (now < (input == kWheelUp ? w.wheel_up_until : w.wheel_down_until))
				return 1.0f;
		}
		return 0;
	}

	// position and motion: one window (no prefix: the one the mouse is in)
	const int source = window_source(window);
	switch (input)
	{
	case kCursorX:
	case kCursorY:
	case kCursorXP:
	case kCursorXN:
	case kCursorYP:
	case kCursorYN:
	{
		float x, y;
		if (!cursor(source, x, y))
			return 0;
		switch (input)
		{
		case kCursorX: return x;
		case kCursorY: return y;
		case kCursorXP: return std::max(0.0f, x);
		case kCursorXN: return std::max(0.0f, -x);
		case kCursorYP: return std::max(0.0f, y);
		default: return std::max(0.0f, -y);
		}
	}
	case kMoveXP:
	case kMoveXN:
	case kMoveYP:
	case kMoveYN:
	{
		const auto& w = m_windows[source];
		if (now - w.last_move > kMoveTimeout)
			return 0;
		int ix, iy, iw, ih;
		LatteRenderTarget_getScreenImageArea(&ix, &iy, &iw, &ih, nullptr, nullptr, source == kPadWindow);
		// 1 = the image's width in half a second
		const float scale = 1.0f / (std::max(1, iw) * 2.0f);
		const float vx = w.vel_x * scale, vy = w.vel_y * scale;
		switch (input)
		{
		case kMoveXP: return std::clamp(vx, 0.0f, 1.0f);
		case kMoveXN: return std::clamp(-vx, 0.0f, 1.0f);
		case kMoveYP: return std::clamp(vy, 0.0f, 1.0f);
		default: return std::clamp(-vy, 0.0f, 1.0f);
		}
	}
	default:
		return 0;
	}
}
