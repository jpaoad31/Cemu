#pragma once

// Mouse-driven controls as explicit bindings (fork addition, see INPUT_REWORK.md "Mouse and pointer
// groups"). Nothing in the emulated controllers reads the mouse directly any more: the GamePad
// touch screen, the GamePad's mouse tilt, the Wii Remote pointer and the Balance Board's centre of
// balance are controls with bindings, and the Mouse device (MouseInputs.h) is one of their sources.
//
// This file has the control ids of those groups, the mouse's default bindings per emulated
// controller (part of the Keyboard/Mouse device's defaults), and the conversion of old profiles
// (whose mouse use was implicit). No Cemu dependencies, so the tests can use it.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MouseBindings
{
	// GamePad (VPADController::ButtonId continues with these)
	enum VPADControl : uint64_t
	{
		// touch screen: position (half axes, like a stick) and "touching"
		kTouchUp = 100,
		kTouchDown,
		kTouchLeft,
		kTouchRight,
		kTouchPress,

		// emulated tilt, used when no device gives motion ("Use motion"): tilt the GamePad by a
		// pointer position (mouse drag), turn it, while "Hold" is active (when bound)
		kTiltUp = 110,
		kTiltDown,
		kTiltLeft,
		kTiltRight,
		kTurnLeft,
		kTurnRight,
		kTiltHold,
	};

	// Wii Remote pointer (IR), like Dolphin's Point group
	enum WiimoteControl : uint64_t
	{
		kPointerUp = 100,
		kPointerDown,
		kPointerLeft,
		kPointerRight,
		kPointerHide,
		kPointerRecenter,
	};

	// Balance Board centre of balance: BalanceBoardController's Balance ids
	enum BoardControl : uint64_t
	{
		kBalanceUp = 2,
		kBalanceDown,
		kBalanceLeft,
		kBalanceRight,
	};

	// Wii Remote buttons the mouse binds by default (WiimoteController::ButtonId)
	constexpr uint64_t kWiimoteA = 1, kWiimoteB = 2;

	// emulated controller type strings (EmulatedController::type_to_string)
	constexpr std::string_view kTypeGamePad = "Wii U GamePad";
	constexpr std::string_view kTypeWiimote = "Wiimote";
	constexpr std::string_view kTypeBoard = "Balance Board";

	// the old separate Mouse device as written in bindings (profiles before version 3)
	constexpr std::string_view kMouseDevice = "Mouse/mouse";
	// the Keyboard/Mouse device
	constexpr std::string_view kKeyboardDevice = "Keyboard/keyboard";

	// (control, mouse input) pairs; mouse ids (MouseInputs::Id), not the Keyboard/Mouse device's
	using Pairs = std::vector<std::pair<uint64_t, uint64_t>>;
	// GamePad: touch = the cursor over the GamePad view + left click there
	Pairs GamePadTouchDefaults();
	// GamePad: tilt = the cursor while the right button is held, turn = the wheel
	Pairs GamePadTiltDefaults();
	// Wii Remote: pointer = the cursor over the TV window, A/B = left/right click there
	Pairs WiimotePointerDefaults();
	// Balance Board: centre of balance = the cursor over the TV window
	Pairs BoardBalanceDefaults();
	// the Mouse device's default bindings for an emulated controller type
	Pairs MouseDefaults(std::string_view type);

	// `Mouse/mouse:<input>` (old profiles)
	std::string MouseInputText(uint64_t input);
	// `<input>` on the Keyboard/Mouse device when it is the selected one: `GamePad Mouse Left`
	std::string CombinedInputText(uint64_t input);
	// `Keyboard/keyboard:<input>`: the mouse on a controller whose device is another one (explicit)
	std::string QualifiedMouseText(uint64_t input);

	// Old profiles (no bindings version 2): what the mouse did implicitly, as bindings to add.
	// - GamePad: touch (left click on the GamePad image) unless touch is bound; mouse tilt (right
	//   drag + wheel) unless the tilt is bound. A motion device ("Use motion") still wins, as before.
	// - Balance Board: centre of balance from the cursor when the Balance controls are unbound and
	//   no real Wii device is assigned (the emulated board followed the mouse then).
	// - Wii Remote / Pro / Classic: nothing (the mouse did nothing for them).
	// bindings: control id -> expression text; has_wii_device: a "Wiimote" API device is attached.
	std::map<uint64_t, std::string> ImplicitMouseBindings(std::string_view type, const std::map<uint64_t, std::string>& bindings, bool has_wii_device);

	// profile <bindings version="..">: 2 = explicit mouse bindings (a separate Mouse device),
	// 3 = one device per emulated controller, unqualified inputs are that device's (see
	// SingleDeviceMigration.h)
	constexpr int kMouseBindingsVersion = 2;
	constexpr int kBindingsVersion = 3;
}
