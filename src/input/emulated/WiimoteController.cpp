#include "input/emulated/WiimoteController.h"

#include "input/api/Controller.h"
#include "input/api/Wiimote/NativeWiimoteController.h"

static_assert((uint64)WiimoteController::kButtonId_A == MouseBindings::kWiimoteA && (uint64)WiimoteController::kButtonId_B == MouseBindings::kWiimoteB,
	"MouseBindings uses WiimoteController's A/B ids");

WiimoteController::WiimoteController(size_t player_index)
	: WPADController(player_index, kDataFormat_CORE_ACC_DPD)
{
}

void WiimoteController::set_device_type(WPADDeviceType device_type)
{
	m_device_type = device_type;
	m_data_format = get_default_data_format();
}

bool WiimoteController::is_mpls_attached()
{
	return m_device_type == kWAPDevMPLS || m_device_type == kWAPDevMPLSClassic || m_device_type == kWAPDevMPLSFreeStyle;
}

uint32 WiimoteController::get_emulated_button_flag(uint32 id) const
{
	return s_get_emulated_button_flag(id);
}

std::vector<std::pair<uint64, uint64>> WiimoteController::get_default_mapping(const std::shared_ptr<ControllerBase>& controller) const
{
	std::vector<std::pair<uint64, uint64>> mapping;
	switch (controller->api())
	{
	case InputAPI::Wiimote: {
		const auto sdl_controller = std::static_pointer_cast<NativeWiimoteController>(controller);
		mapping =
		{
			{kButtonId_A, kWiimoteButton_A},
			{kButtonId_B, kWiimoteButton_B},
			{kButtonId_1, kWiimoteButton_One},
			{kButtonId_2, kWiimoteButton_Two},

			{kButtonId_Home, kWiimoteButton_Home},

			{kButtonId_Plus, kWiimoteButton_Plus},
			{kButtonId_Minus, kWiimoteButton_Minus},

			{kButtonId_Up, kWiimoteButton_Up},
			{kButtonId_Down, kWiimoteButton_Down},
			{kButtonId_Left, kWiimoteButton_Left},
			{kButtonId_Right, kWiimoteButton_Right},

			{kButtonId_Nunchuck_Z, kWiimoteButton_Z},
			{kButtonId_Nunchuck_C, kWiimoteButton_C},

			{kButtonId_Nunchuck_Up, kAxisYP},
			{kButtonId_Nunchuck_Down, kAxisYN},
			{kButtonId_Nunchuck_Left, kAxisXN},
			{kButtonId_Nunchuck_Right, kAxisXP},
		};
		break;
	}
	case InputAPI::Mouse:
		mapping = MouseBindings::MouseDefaults(MouseBindings::kTypeWiimote);
		break;
	default:
		break;
	}

	return mapping;
}

glm::vec2 WiimoteController::get_axis() const
{
	const auto left = get_axis_value(kButtonId_Nunchuck_Left);
	const auto right = get_axis_value(kButtonId_Nunchuck_Right);

	const auto up = get_axis_value(kButtonId_Nunchuck_Up);
	const auto down = get_axis_value(kButtonId_Nunchuck_Down);

	glm::vec2 result;
	result.x = (left > right) ? -left : right;
	result.y = (up > down) ? up : -down;
	return result;
}

glm::vec2 WiimoteController::get_rotation() const
{
	return {};
}

glm::vec2 WiimoteController::get_trigger() const
{
	return {};
}

void WiimoteController::load(const pugi::xml_node& node)
{
	base_type::load(node);

	if (const auto value = node.child("device_type"))
		m_device_type = ConvertString<WPADDeviceType>(value.child_value());

	if (const auto pointer = node.child("pointer"))
	{
		PointerSettings settings;
		settings.center_x = pointer.attribute("center_x").as_float(0);
		settings.center_y = pointer.attribute("center_y").as_float(0);
		settings.width = pointer.attribute("width").as_float(1);
		settings.height = pointer.attribute("height").as_float(1);
		settings.relative = pointer.attribute("relative").as_bool(false);
		settings.auto_hide = pointer.attribute("auto_hide").as_bool(false);
		set_pointer_settings(settings);
	}
}

void WiimoteController::save(pugi::xml_node& node)
{
	base_type::save(node);

	node.append_child("device_type").append_child(pugi::node_pcdata).set_value(fmt::format("{}", (int)m_device_type).c_str());

	const auto settings = get_pointer_settings();
	auto pointer = node.append_child("pointer");
	pointer.append_attribute("center_x") = settings.center_x;
	pointer.append_attribute("center_y") = settings.center_y;
	pointer.append_attribute("width") = settings.width;
	pointer.append_attribute("height") = settings.height;
	pointer.append_attribute("relative") = settings.relative;
	pointer.append_attribute("auto_hide") = settings.auto_hide;
}

WiimoteController::PointerSettings WiimoteController::get_pointer_settings() const
{
	std::scoped_lock lock(m_pointer_mutex);
	return m_pointer_settings;
}

void WiimoteController::set_pointer_settings(const PointerSettings& settings)
{
	std::scoped_lock lock(m_pointer_mutex);
	m_pointer_settings = settings;
	m_pointer_settings.center_x = std::clamp(settings.center_x, -1.0f, 1.0f);
	m_pointer_settings.center_y = std::clamp(settings.center_y, -1.0f, 1.0f);
	m_pointer_settings.width = std::clamp(settings.width, 0.05f, 4.0f);
	m_pointer_settings.height = std::clamp(settings.height, 0.05f, 4.0f);
}

WiimoteController::PointerState WiimoteController::get_pointer()
{
	PointerState result;
	for (uint64 id = kButtonId_Pointer_Up; id <= kButtonId_Pointer_Right; ++id)
		result.bound |= has_mapping(id);
	if (!result.bound)
		return result;

	const auto half_axes = [](float negative, float positive) { return std::clamp(negative > positive ? -negative : positive, -1.0f, 1.0f); };
	const glm::vec2 input{half_axes(get_axis_value(kButtonId_Pointer_Left), get_axis_value(kButtonId_Pointer_Right)),
		half_axes(get_axis_value(kButtonId_Pointer_Up), get_axis_value(kButtonId_Pointer_Down))};
	const bool hide = is_mapping_down(kButtonId_Pointer_Hide);
	const bool recenter = is_mapping_down(kButtonId_Pointer_Recenter);

	std::scoped_lock lock(m_pointer_mutex);
	const auto& settings = m_pointer_settings;
	const auto now = std::chrono::steady_clock::now();
	glm::vec2 value = input;
	if (settings.relative)
	{
		// the binding is a speed: a full deflection crosses the screen in a second
		if (m_pointer_step != std::chrono::steady_clock::time_point{})
		{
			const float dt = std::min(0.1f, std::chrono::duration<float>(now - m_pointer_step).count());
			m_pointer_relative = glm::clamp(m_pointer_relative + input * (2.0f * dt), glm::vec2(-1.0f), glm::vec2(1.0f));
		}
		if (recenter)
			m_pointer_relative = {};
		value = m_pointer_relative;
	}
	m_pointer_step = now;

	// -1..1 on the screen (KPAD pos), then 0..1
	const glm::vec2 screen{settings.center_x + value.x * settings.width, settings.center_y + value.y * settings.height};
	const glm::vec2 position = glm::clamp((screen + 1.0f) * 0.5f, glm::vec2(0.0f), glm::vec2(1.0f));
	if (glm::length(position - m_pointer_last) > 0.002f || m_pointer_moved == std::chrono::steady_clock::time_point{})
		m_pointer_moved = now;
	m_pointer_prev = m_pointer_last;
	m_pointer_last = position;

	result.position = position;
	result.visible = !hide && !(settings.auto_hide && now - m_pointer_moved > std::chrono::seconds(2));
	return result;
}

WPADController::PointerSample WiimoteController::get_kpad_pointer()
{
	// a real Wii Remote's IR camera first (while it sees the sensor bar)
	auto sample = base_type::get_kpad_pointer();
	if (sample.visibility != PositionVisibility::NONE)
		return sample;
	const auto pointer = get_pointer();
	if (!pointer.bound || !pointer.visible)
		return {};
	PointerSample result;
	result.visibility = PositionVisibility::FULL;
	result.position = pointer.position;
	{
		std::scoped_lock lock(m_pointer_mutex);
		result.previous = m_pointer_prev;
	}
	return result;
}

uint32 WiimoteController::s_get_emulated_button_flag(uint32 id)
{
	switch (id)
	{
		case kButtonId_A:
			return kWPADButton_A;
		case kButtonId_B:
			return kWPADButton_B;
		case kButtonId_1:
			return kWPADButton_1;
		case kButtonId_2:
			return kWPADButton_2;

		case kButtonId_Plus:
			return kWPADButton_Plus;
		case kButtonId_Minus:
			return kWPADButton_Minus;
		case kButtonId_Home:
			return kWPADButton_Home;

		case kButtonId_Up:
			return kWPADButton_Up;
		case kButtonId_Down:
			return kWPADButton_Down;
		case kButtonId_Left:
			return kWPADButton_Left;
		case kButtonId_Right:
			return kWPADButton_Right;

		case kButtonId_Nunchuck_Z:
			return kWPADButton_Z;
		case kButtonId_Nunchuck_C:
			return kWPADButton_C;
	}

	return 0;
}

std::string_view WiimoteController::get_button_name(ButtonId id)
{
	switch (id)
	{
	case kButtonId_A: return "A";
	case kButtonId_B: return "B";
	case kButtonId_1: return "1";
	case kButtonId_2: return "2";

	case kButtonId_Home: return TR_NOOP("home");
	case kButtonId_Plus: return "+";
	case kButtonId_Minus: return "-";

	case kButtonId_Up: return TR_NOOP("up");
	case kButtonId_Down: return TR_NOOP("down");
	case kButtonId_Left: return TR_NOOP("left");
	case kButtonId_Right: return TR_NOOP("right");

	case kButtonId_Nunchuck_Z: return "Z";
	case kButtonId_Nunchuck_C: return "C";

	case kButtonId_Nunchuck_Up: return TR_NOOP("up");
	case kButtonId_Nunchuck_Down: return TR_NOOP("down");
	case kButtonId_Nunchuck_Left: return TR_NOOP("left");
	case kButtonId_Nunchuck_Right: return TR_NOOP("right");

	case kButtonId_Pointer_Up: return TR_NOOP("up");
	case kButtonId_Pointer_Down: return TR_NOOP("down");
	case kButtonId_Pointer_Left: return TR_NOOP("left");
	case kButtonId_Pointer_Right: return TR_NOOP("right");
	case kButtonId_Pointer_Hide: return TR_NOOP("hide");
	case kButtonId_Pointer_Recenter: return TR_NOOP("recenter");

	default:
		return "";
	}
}


