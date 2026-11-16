#pragma once

// The mouse as Cemu's game windows see it (fork addition, see INPUT_REWORK.md "Mouse and pointer
// groups"), for the Mouse device (api/Mouse). The windows report moves, clicks, touches and the
// wheel here; nothing else in the emulation reads the mouse.

#include "input/api/Mouse/MouseInputs.h"

#include <array>
#include <chrono>
#include <mutex>

class MouseState
{
public:
	// where an event came from
	enum Source
	{
		kMainWindow,
		kPadWindow,
		// the input settings window (any click while it is open, so Detect and the live fields see
		// clicks: the game windows don't get events while it is modal)
		kSettings,
		kSourceCount,
	};

	static MouseState& Get();

	void on_move(Source source, int x, int y); // physical pixels in the window's canvas
	void on_button(Source source, int button, bool down, int x, int y); // button: MouseInputs::kLeft..kForward
	void on_wheel(Source source, float notches); // + up
	// the settings window's view of the buttons (polled; mask of MouseInputs buttons); active = false
	// when it closes
	void set_settings_buttons(uint32_t mask, bool active);

	// the value of a MouseInputs input (0..1, Cursor X/Y -1..1) and whether it is pressed
	float value(uint64_t id) const;
	bool pressed(uint64_t id) const;

	// a button of one window, as the window saw it (the imgui overlay)
	bool button_down(Source source, int button) const;

	// the window an input refers to (main or pad), or -1 if none
	int window_source(MouseInputs::Window window) const;

private:
	using Clock = std::chrono::steady_clock;
	struct Window
	{
		int x = 0, y = 0;
		bool has_position = false;
		std::array<bool, MouseInputs::kButtonCount> down{};
		std::array<Clock::time_point, MouseInputs::kButtonCount> pressed_at{}, hold_until{};
		Clock::time_point wheel_up_until{}, wheel_down_until{};
		Clock::time_point last_move{};
		float vel_x = 0, vel_y = 0; // pixels per second
	};

	bool button_active(const Window& w, size_t button, Clock::time_point now) const;
	// cursor over a window's image: -1..1
	bool cursor(int source, float& x, float& y) const;

	mutable std::mutex m_mutex;
	std::array<Window, kSourceCount> m_windows{};
	bool m_settings_active = false;
	int m_last_window = kMainWindow; // the game window the mouse was last in
};
