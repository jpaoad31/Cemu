#pragma once

#include "input/api/Mouse/MouseControllerProvider.h"
#include "input/api/Controller.h"

// The mouse over Cemu's game windows as a device with bindable inputs (fork addition, see
// MouseInputs.h for the inputs and INPUT_REWORK.md). One device, uuid "mouse", always available.
class MouseController : public Controller<MouseControllerProvider>
{
public:
	MouseController();

	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::Mouse) == "Mouse");
		return to_string(InputAPI::Mouse);
	}
	InputAPI::Type api() const override { return InputAPI::Mouse; }

	bool is_connected() override { return true; }
	// its values come from get_axis_value, not from stick axes
	bool has_axis() const override { return false; }

	// nothing to calibrate: a button held while the device is added must not be ignored for good
	const ControllerState& calibrate() override;

	float get_axis_value(uint64 button) const override;
	bool is_input_active(uint64 button) const override;

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;
};
