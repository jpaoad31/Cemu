#pragma once

#include "input/api/Keyboard/KeyboardControllerProvider.h"
#include "input/api/Controller.h"

// The "Keyboard/Mouse" device (fork: the keyboard and the mouse are one device, as in Dolphin; see
// INPUT_REWORK.md). Keys keep their key codes as ids; the mouse over Cemu's game windows (buttons,
// wheel, cursor, motion: input/api/Mouse/MouseInputs.h) has ids from MouseInputs::kKeyboardBase.
// API "Keyboard", uuid "keyboard" as before, so old profiles' keyboard bindings stay valid.
class KeyboardController : public Controller<KeyboardControllerProvider>
{
public:
	KeyboardController();

	std::string_view api_name() const override // TODO: use constexpr virtual function with c++20
	{
		static_assert(to_string(InputAPI::Keyboard) == "Keyboard");
		return to_string(InputAPI::Keyboard);
	}
	InputAPI::Type api() const override { return InputAPI::Keyboard; }

	bool is_connected() override { return true; }

	bool has_axis() const override { return false; }

	// nothing to calibrate: a key or click held while the device is selected must not be ignored for good
	const ControllerState& calibrate() override;

	float get_axis_value(uint64 button) const override;
	bool is_input_active(uint64 button) const override;

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

};
