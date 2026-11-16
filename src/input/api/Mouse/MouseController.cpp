#include "input/api/Mouse/MouseController.h"

#include "input/api/Mouse/MouseInputs.h"
#include "input/MouseState.h"

std::vector<std::shared_ptr<ControllerBase>> MouseControllerProvider::get_controllers()
{
	return {std::make_shared<MouseController>()};
}

MouseController::MouseController()
	: base_type("mouse", "Mouse")
{
}

const ControllerState& MouseController::calibrate()
{
	m_default_state = {};
	m_is_calibrated = true;
	return m_default_state;
}

std::string MouseController::get_button_name(uint64 button) const
{
	return MouseInputs::Name(button);
}

float MouseController::get_axis_value(uint64 button) const
{
	return MouseState::Get().value(button);
}

bool MouseController::is_input_active(uint64 button) const
{
	// buttons and the wheel as of the last state update (what Detect compares); the cursor and
	// motion are active past half way
	if (MouseInputs::IsPressable(button))
		return m_last_state.buttons.GetButtonState((uint32)button);
	return std::abs(MouseState::Get().value(button)) > 0.5f;
}

ControllerState MouseController::raw_state()
{
	ControllerState result{};
	const auto& mouse = MouseState::Get();
	std::vector<uint32> pressed;
	for (const auto id : MouseInputs::AllIds())
	{
		if (MouseInputs::IsPressable(id) && mouse.pressed(id))
			pressed.emplace_back((uint32)id);
	}
	result.buttons.SetPressedButtons(pressed);
	return result;
}
