#include <boost/container/small_vector.hpp>

#include "input/api/Keyboard/KeyboardController.h"
#include "input/api/Mouse/MouseInputs.h"
#include "input/MouseState.h"
#include "WindowSystem.h"

KeyboardController::KeyboardController()
	: base_type("keyboard", "Keyboard/Mouse")
{
	
}

const ControllerState& KeyboardController::calibrate()
{
	m_default_state = {};
	m_is_calibrated = true;
	return m_default_state;
}

std::string KeyboardController::get_button_name(uint64 button) const
{
	if (MouseInputs::IsKeyboardMouseId(button))
		return MouseInputs::CombinedName(MouseInputs::MouseIdOf(button));
	return WindowSystem::GetKeyCodeName(button);
}

float KeyboardController::get_axis_value(uint64 button) const
{
	if (MouseInputs::IsKeyboardMouseId(button))
		return MouseState::Get().value(MouseInputs::MouseIdOf(button));
	return ControllerBase::get_axis_value(button);
}

bool KeyboardController::is_input_active(uint64 button) const
{
	// mouse buttons and the wheel as of the last state update (what Detect compares); the cursor
	// and motion are active past half way
	if (MouseInputs::IsKeyboardMouseId(button) && !MouseInputs::IsPressable(MouseInputs::MouseIdOf(button)))
		return std::abs(MouseState::Get().value(MouseInputs::MouseIdOf(button))) > 0.5f;
	return ControllerBase::is_input_active(button);
}

ControllerState KeyboardController::raw_state()
{
	ControllerState result{};

	if (WindowSystem::GetWindowInfo().debugger_focused)
		return result;

	boost::container::small_vector<uint32, 16> pressed;
	WindowSystem::GetWindowInfo().iter_keystates([&pressed](const std::pair<const uint32, bool>& keyState) { if (keyState.second) pressed.emplace_back(keyState.first); });
	const auto& mouse = MouseState::Get();
	for (const auto id : MouseInputs::AllIds())
	{
		if (MouseInputs::IsPressable(id) && mouse.pressed(id))
			pressed.emplace_back((uint32)MouseInputs::KeyboardId(id));
	}
	result.buttons.SetPressedButtons(pressed);
	return result;
}
