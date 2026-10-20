#include "input/emulated/BalanceBoardController.h"

#include "input/api/Controller.h"
#include "input/InputManager.h"
#include "Cafe/HW/Latte/Core/Latte.h"

BalanceBoardController::BalanceBoardController(size_t player_index)
	: WPADController(player_index, kDataFormat_BALANCE_CHECKER), m_ext_memory(BalanceBoardMath::BuildExtensionMemory())
{
}

uint32 BalanceBoardController::get_emulated_button_flag(uint32 id) const
{
	// the board's front (power) button is reported as A
	return id == kButtonId_Power ? kWPADButton_A : 0;
}

bool BalanceBoardController::set_default_mapping(const std::shared_ptr<ControllerBase>& controller)
{
	std::vector<std::pair<uint64, uint64>> mapping;
	switch (controller->api())
	{
	case InputAPI::Keyboard:
	{
		// O step off, P power (as Dolphin's emulated board). The Balance stick is left unmapped:
		// it then follows the mouse (see get_axis); map it to keys or a stick to override that
#if BOOST_OS_WINDOWS
		constexpr uint64 kKeyO = 'O', kKeyP = 'P';
#elif BOOST_OS_MACOS
		// macOS virtual key codes (kVK_ANSI_*)
		constexpr uint64 kKeyO = 0x1F, kKeyP = 0x23;
#else
		// GDK key values
		constexpr uint64 kKeyO = 'o', kKeyP = 'p';
#endif
		mapping =
		{
			{kButtonId_Power, kKeyP},
			{kButtonId_StepOffToggle, kKeyO},
		};
		break;
	}
	case InputAPI::SDLController:
		mapping =
		{
			{kButtonId_Power, kButton0},
			{kButtonId_Balance_Up, kAxisYN},
			{kButtonId_Balance_Down, kAxisYP},
			{kButtonId_Balance_Left, kAxisXN},
			{kButtonId_Balance_Right, kAxisXP},
			{kButtonId_StepOffToggle, kButton3},
		};
		break;
	case InputAPI::XInput:
		mapping =
		{
			{kButtonId_Power, kButton13},
			{kButtonId_Balance_Up, kAxisYP},
			{kButtonId_Balance_Down, kAxisYN},
			{kButtonId_Balance_Left, kAxisXN},
			{kButtonId_Balance_Right, kAxisXP},
			{kButtonId_StepOffToggle, kButton14},
		};
		break;
	default:
		break;
	}

	bool mapping_updated = false;
	for (const auto& m : mapping)
	{
		if (m_mappings.find(m.first) == m_mappings.cend())
		{
			set_mapping(m.first, controller, m.second);
			mapping_updated = true;
		}
	}
	return mapping_updated;
}

bool BalanceBoardController::is_balance_mapped() const
{
	for (uint64 id = kButtonId_Balance_Up; id <= kButtonId_Balance_Right; id++)
	{
		if (get_mapping_controller(id))
			return true;
	}
	return false;
}

// With no Balance mapping, the centre of balance follows the mouse over the TV image in the main
// window, measured the same way as the GamePad touch screen (VPADController::update_touch): the
// image centre is centred, its edges are a full lean, up is front
glm::vec2 BalanceBoardController::get_mouse_axis()
{
	glm::ivec2 image_pos, image_size;
	LatteRenderTarget_getScreenImageArea(&image_pos.x, &image_pos.y, &image_size.x, &image_size.y, nullptr, nullptr, false);
	if (image_size.x <= 0 || image_size.y <= 0)
		return {};

	glm::vec2 relative_mouse_pos = InputManager::instance().get_mouse_position(false) - image_pos;
	relative_mouse_pos = { std::min(relative_mouse_pos.x, (float)image_size.x), std::min(relative_mouse_pos.y, (float)image_size.y) };
	relative_mouse_pos = { std::max(relative_mouse_pos.x, 0.0f), std::max(relative_mouse_pos.y, 0.0f) };
	relative_mouse_pos /= image_size;

	return { relative_mouse_pos.x * 2.0f - 1.0f, 1.0f - relative_mouse_pos.y * 2.0f };
}

glm::vec2 BalanceBoardController::get_axis() const
{
	if (!is_balance_mapped())
		return get_mouse_axis();

	const auto left = get_axis_value(kButtonId_Balance_Left);
	const auto right = get_axis_value(kButtonId_Balance_Right);
	const auto up = get_axis_value(kButtonId_Balance_Up);
	const auto down = get_axis_value(kButtonId_Balance_Down);

	glm::vec2 result;
	result.x = (left > right) ? -left : right;
	result.y = (up > down) ? up : -down;
	// square gate like Dolphin's Balance stick
	result.x = std::clamp(result.x, -1.0f, 1.0f);
	result.y = std::clamp(result.y, -1.0f, 1.0f);
	return result;
}

void BalanceBoardController::update_step_off()
{
	const bool toggle_down = is_mapping_down(kButtonId_StepOffToggle);
	bool expected = !toggle_down;
	// only the caller that sees the key change flips the toggle
	if (m_step_off_toggle_was_down.compare_exchange_strong(expected, toggle_down) && toggle_down)
	{
		const bool off = !m_step_off_toggled.load();
		m_step_off_toggled.store(off);
		cemuLog_log(LogType::InputAPI, "BalanceBoard: step off toggle -> {}", off ? "off the board" : "on the board");
	}
	m_step_off_hold.store(is_mapping_down(kButtonId_StepOffHold));
}

std::array<uint16, BalanceBoardMath::kSensorCount> BalanceBoardController::get_sensor_values()
{
	update_step_off();
	return compute_sensor_values(is_stepped_off());
}

std::array<uint16, BalanceBoardMath::kSensorCount> BalanceBoardController::compute_sensor_values(bool stepped_off) const
{
	std::array<uint16, BalanceBoardMath::kSensorCount> raw{};
	const double total = stepped_off ? 0.0 : m_weight;

	// a full stick deflection moves the centre of balance by the foot separation/length,
	// relative to the distance between the load cells
	const auto axis = get_axis();
	const double x = axis.x * (m_foot_separation / BalanceBoardMath::kSensorDistanceX);
	const double y = axis.y * (m_foot_length / BalanceBoardMath::kSensorDistanceY);
	const auto sensors = BalanceBoardMath::CenterOfBalanceToSensors(x, y, total);
	for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
		raw[i] = BalanceBoardMath::KgToRaw(sensors[i]);
	return raw;
}

void BalanceBoardController::fill_balance_status(WPADBLStatus_t& status)
{
	const auto raw = get_sensor_values();
	for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
		status.press[i] = raw[i];
	status.temp = BalanceBoardMath::kReferenceTemperature;
	status.battery = get_battery_raw();
}

void BalanceBoardController::load(const pugi::xml_node& node)
{
	base_type::load(node);

	if (const auto value = node.child("weight"))
		set_weight(ConvertString<double>(value.child_value()));
	if (const auto value = node.child("foot_separation"))
		set_foot_separation(ConvertString<double>(value.child_value()));
	if (const auto value = node.child("foot_length"))
		set_foot_length(ConvertString<double>(value.child_value()));
	if (const auto value = node.child("battery"))
		set_battery(ConvertString<sint32>(value.child_value()));
}

void BalanceBoardController::save(pugi::xml_node& node)
{
	base_type::save(node);

	node.append_child("weight").append_child(pugi::node_pcdata).set_value(fmt::format("{}", m_weight).c_str());
	node.append_child("foot_separation").append_child(pugi::node_pcdata).set_value(fmt::format("{}", m_foot_separation).c_str());
	node.append_child("foot_length").append_child(pugi::node_pcdata).set_value(fmt::format("{}", m_foot_length).c_str());
	node.append_child("battery").append_child(pugi::node_pcdata).set_value(fmt::format("{}", m_battery).c_str());
}

std::string_view BalanceBoardController::get_button_name(ButtonId id)
{
	switch (id)
	{
	case kButtonId_Power: return TR_NOOP("Power");
	case kButtonId_Balance_Up: return TR_NOOP("front");
	case kButtonId_Balance_Down: return TR_NOOP("back");
	case kButtonId_Balance_Left: return TR_NOOP("left");
	case kButtonId_Balance_Right: return TR_NOOP("right");
	case kButtonId_StepOffToggle: return TR_NOOP("Step off (toggle)");
	case kButtonId_StepOffHold: return TR_NOOP("Step off (hold)");
	default:
		return "";
	}
}
