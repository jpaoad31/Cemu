#pragma once

// The mouse's inputs (fork addition, see INPUT_REWORK.md "Keyboard/Mouse"). The mouse is part of
// the "Keyboard/Mouse" device (KeyboardController), as in Dolphin: its inputs are that device's
// inputs from kKeyboardBase up. No Cemu dependencies, so the binding tests can name them.
//
// Every input exists for three "windows":
//   ""         (no prefix) the Cemu game window the mouse is in (or was last in)
//   "TV "      the main window (the TV image)
//   "GamePad " the GamePad view: the GamePad window when it is open, else the main window
// and is one of:
//   buttons   Mouse Left, Mouse Right, Mouse Middle, Mouse Back, Mouse Forward (pressed in that
//             window; a click lasts >= 50 ms)
//   wheel     Wheel Up, Wheel Down (active for 100 ms per notch)
//   cursor    Cursor X, Cursor Y: absolute position over the window's image, -1..1, Y down;
//             Cursor X+/X-/Y+/Y-: the half axes, 0..1
//   motion    Move X+/X-/Y+/Y-: relative motion (speed; 1 = the image's width in half a second)
// Only buttons and the wheel are "pressed" (Detect catches clicks and the wheel, not moves).
//
// Two numberings: the mouse's own ids (Id(): window * 32 + input; MouseState uses them) and the
// Keyboard/Mouse device's ids (KeyboardId(): kKeyboardBase + the mouse id; keys are below it).
// Name()/FromName() are the names of the old separate "Mouse" device (buttons without "Mouse ",
// e.g. "GamePad Left"), kept to convert old profiles.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MouseInputs
{
	enum Window : uint64_t
	{
		kAny = 0,
		kTV = 1,
		kGamePad = 2,
		kWindowCount = 3,
	};

	enum Input : uint64_t
	{
		kLeft,
		kRight,
		kMiddle,
		kBack,
		kForward,
		kWheelUp,
		kWheelDown,
		kCursorX,
		kCursorY,
		kCursorXP,
		kCursorXN,
		kCursorYP,
		kCursorYN,
		kMoveXP,
		kMoveXN,
		kMoveYP,
		kMoveYN,
		kInputCount,
	};

	constexpr uint64_t kWindowStride = 32;
	constexpr uint64_t kButtonCount = 5;

	constexpr uint64_t Id(Window window, Input input) { return (uint64_t)window * kWindowStride + (uint64_t)input; }
	constexpr Window WindowOf(uint64_t id) { return (Window)(id / kWindowStride); }
	constexpr Input InputOf(uint64_t id) { return (Input)(id % kWindowStride); }
	constexpr bool IsValid(uint64_t id) { return id / kWindowStride < kWindowCount && id % kWindowStride < kInputCount; }
	constexpr bool IsButton(uint64_t id) { return IsValid(id) && InputOf(id) < kButtonCount; }
	constexpr bool IsWheel(uint64_t id) { return IsValid(id) && (InputOf(id) == kWheelUp || InputOf(id) == kWheelDown); }
	// can be pressed (buttons, wheel); the rest are analogue
	constexpr bool IsPressable(uint64_t id) { return IsButton(id) || IsWheel(id); }

	inline std::string_view InputName(Input input)
	{
		constexpr std::string_view kNames[kInputCount] = {
			"Left", "Right", "Middle", "Back", "Forward", "Wheel Up", "Wheel Down",
			"Cursor X", "Cursor Y", "Cursor X+", "Cursor X-", "Cursor Y+", "Cursor Y-",
			"Move X+", "Move X-", "Move Y+", "Move Y-",
		};
		return input < kInputCount ? kNames[input] : std::string_view{};
	}

	inline std::string_view WindowPrefix(Window window)
	{
		switch (window)
		{
		case kTV: return "TV ";
		case kGamePad: return "GamePad ";
		default: return "";
		}
	}

	inline std::string Name(uint64_t id)
	{
		if (!IsValid(id))
			return "Mouse " + std::to_string(id);
		return std::string(WindowPrefix(WindowOf(id))) + std::string(InputName(InputOf(id)));
	}

	// case-insensitive
	inline std::optional<uint64_t> FromName(std::string_view name)
	{
		const auto iequals = [](std::string_view a, std::string_view b) {
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
			{
				char x = a[i], y = b[i];
				if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
				if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
				if (x != y)
					return false;
			}
			return true;
		};
		for (uint64_t w = 0; w < kWindowCount; ++w)
		{
			for (uint64_t i = 0; i < kInputCount; ++i)
			{
				const uint64_t id = Id((Window)w, (Input)i);
				if (iequals(Name(id), name))
					return id;
			}
		}
		return std::nullopt;
	}

	// every input, the window-specific ones first (Detect reports a click in the GamePad window as
	// "GamePad Left", which is what a touch binding wants)
	inline std::vector<uint64_t> AllIds()
	{
		std::vector<uint64_t> ids;
		for (uint64_t w : {kTV, kGamePad, kAny})
		{
			for (uint64_t i = 0; i < kInputCount; ++i)
				ids.push_back(Id((Window)w, (Input)i));
		}
		return ids;
	}

	// ---- the Keyboard/Mouse device ----

	// above every key code (Windows VK, macOS key codes, GDK key values < 0x11000000)
	constexpr uint64_t kKeyboardBase = 0x40000000;
	constexpr uint64_t KeyboardId(uint64_t mouse_id) { return kKeyboardBase + mouse_id; }
	constexpr bool IsKeyboardMouseId(uint64_t id) { return id >= kKeyboardBase && IsValid(id - kKeyboardBase); }
	constexpr uint64_t MouseIdOf(uint64_t keyboard_id) { return keyboard_id - kKeyboardBase; }

	inline std::string_view CombinedInputName(Input input)
	{
		constexpr std::string_view kNames[kInputCount] = {
			"Mouse Left", "Mouse Right", "Mouse Middle", "Mouse Back", "Mouse Forward", "Wheel Up", "Wheel Down",
			"Cursor X", "Cursor Y", "Cursor X+", "Cursor X-", "Cursor Y+", "Cursor Y-",
			"Move X+", "Move X-", "Move Y+", "Move Y-",
		};
		return input < kInputCount ? kNames[input] : std::string_view{};
	}

	// the Keyboard/Mouse device's name of a mouse input (its mouse id): "GamePad Mouse Left"
	inline std::string CombinedName(uint64_t mouse_id)
	{
		if (!IsValid(mouse_id))
			return "Mouse " + std::to_string(mouse_id);
		return std::string(WindowPrefix(WindowOf(mouse_id))) + std::string(CombinedInputName(InputOf(mouse_id)));
	}

	// a Keyboard/Mouse input name -> the device's id (kKeyboardBase + mouse id); case-insensitive
	inline std::optional<uint64_t> FromCombinedName(std::string_view name)
	{
		for (uint64_t w = 0; w < kWindowCount; ++w)
		{
			for (uint64_t i = 0; i < kInputCount; ++i)
			{
				const uint64_t id = Id((Window)w, (Input)i);
				const auto n = CombinedName(id);
				if (n.size() != name.size())
					continue;
				bool same = true;
				for (size_t k = 0; k < n.size() && same; ++k)
				{
					char x = n[k], y = name[k];
					if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
					if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
					same = x == y;
				}
				if (same)
					return KeyboardId(id);
			}
		}
		return std::nullopt;
	}
}
