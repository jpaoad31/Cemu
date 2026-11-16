#pragma once

// One device per emulated controller (fork addition, see INPUT_REWORK.md "One device per
// controller"): converting profiles saved before bindings version 3, when an emulated controller
// had a list of devices whose bindings were OR'ed together.
//
// The rule (the user's decision: "just keep the bound device and drop the rest"):
// - Keyboard and the old Mouse device are one device now, "Keyboard/Mouse".
// - The kept device is the main non-keyboard device (Remote GamePad, a real Wii device, an SDL/DSU
//   pad...) if the profile has one, else Keyboard/Mouse. With several non-keyboard devices: the one
//   with the most inputs bound, then Remote GamePad > Wii device > others, then profile order.
// - Plain bindings (lists of inputs) keep the kept device's inputs, now written without a device
//   (`X`), and drop every other device's inputs. Advanced expressions (anything but a plain list)
//   are kept whole; their other devices' inputs stay explicit (`RemoteGamePad/26770:A`).
// - The old Mouse device's input names become the Keyboard/Mouse names ("GamePad Left" ->
//   "GamePad Mouse Left").
//
// No Cemu dependencies (InputExpression, MouseInputs only), so InputExpressionTest covers it.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SingleDeviceMigration
{
	// a <controller> node of an old profile
	struct ProfileDevice
	{
		std::string api;  // "Keyboard", "Mouse", "RemoteGamePad", ...
		std::string uuid;
		std::string display_name;
	};

	bool IsKeyboardOrMouse(std::string_view api);

	// the device to keep (index into devices); nullopt when there are none
	std::optional<size_t> ChooseDevice(const std::vector<ProfileDevice>& devices, const std::map<uint64_t, std::string>& bindings);

	struct Dropped
	{
		std::string device; // display name ("Keyboard/Mouse" for the keyboard and the mouse)
		size_t inputs = 0;  // inputs removed from bindings (0: the device had none)
	};

	struct Result
	{
		// the selected device: the kept one ({"Keyboard", "keyboard", "Keyboard/Mouse"} for the
		// keyboard and/or the mouse); nullopt without devices
		std::optional<ProfileDevice> device;
		std::map<uint64_t, std::string> bindings; // version 3 texts
		std::vector<Dropped> dropped;             // the other devices
		size_t advanced_kept = 0;                 // advanced expressions that keep another device explicitly
		size_t dropped_inputs() const;
	};

	Result Migrate(const std::vector<ProfileDevice>& devices, const std::map<uint64_t, std::string>& bindings);

	// for the input settings and the log: "Bindings from Keyboard/Mouse (12) were removed: one
	// device per controller." Empty when nothing was dropped.
	std::string Note(const Result& result);

	constexpr std::string_view kKeyboardMouseName = "Keyboard/Mouse";
}
