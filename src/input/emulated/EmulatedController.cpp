#include "input/emulated/EmulatedController.h"

#include "input/api/Controller.h"
#include "input/api/Mouse/MouseInputs.h"
#include "input/emulated/MouseBindings.h"
#include "input/ControllerFactory.h"
#include "input/InputPresets.h"

#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

std::string_view EmulatedController::type_to_string(Type type)
{
	switch (type)
	{
	case VPAD: return "Wii U GamePad";
	case Pro: return "Wii U Pro Controller";
	case Classic: return "Wii U Classic Controller";
	case Wiimote: return "Wiimote";
	case BalanceBoard: return "Balance Board";
	}

	throw std::runtime_error(fmt::format("unknown emulated controller: {}", stdx::to_underlying(type)));
}

EmulatedController::Type EmulatedController::type_from_string(std::string_view str)
{
	if (str == "Wii U GamePad")
		return VPAD;
	else if (str == "Wii U Pro Controller")
		return Pro;
	else if (str == "Wii U Classic Controller")
		return Classic;
	else if (str == "Wiimote")
		return Wiimote;
	else if (str == "Balance Board")
		return BalanceBoard;

	throw std::runtime_error(fmt::format("unknown emulated controller: {}", str));
}

EmulatedController::EmulatedController(size_t player_index)
	: m_player_index(player_index)
{
}

void EmulatedController::calibrate()
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->calibrate();
	}
}

void EmulatedController::connect()
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->connect();
	}
}

void EmulatedController::update()
{
	std::shared_lock lock(m_mutex);
	for(const auto& controller : m_controllers)
	{
		controller->update();
	}
}

void EmulatedController::controllers_update_states()
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->update_state();
	}
}

void EmulatedController::controllers_read_done()
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->on_emulated_read_done();
	}
}

// The device's own data (rumble, battery, motion, pointer/touch position) comes from the selected
// device only: devices named explicitly in bindings only give their inputs.

void EmulatedController::start_rumble()
{
	m_rumble = true;
	std::shared_lock lock(m_mutex);
	if (m_device)
		m_device->start_rumble();
}

void EmulatedController::stop_rumble()
{
	if (!m_rumble)
		return;

	m_rumble = false;

	std::shared_lock lock(m_mutex);
	if (m_device)
		m_device->stop_rumble();
}

bool EmulatedController::is_battery_low() const
{
	std::shared_lock lock(m_mutex);
	return m_device && m_device->has_low_battery();
}

bool EmulatedController::has_motion() const
{
	std::shared_lock lock(m_mutex);
	return m_device && m_device->use_motion();
}

MotionSample EmulatedController::get_motion_data() const
{
	std::shared_lock lock(m_mutex);
	if (m_device && m_device->use_motion())
		return m_device->get_motion_sample();
	return {};
}

bool EmulatedController::has_second_motion() const
{
	// a Wii Remote's Nunchuk
#ifdef SUPPORTS_WIIMOTE
	std::shared_lock lock(m_mutex);
	if (m_device && m_device->use_motion() && m_device->api() == InputAPI::Wiimote)
		return ((NativeWiimoteController*)m_device.get())->get_extension() == NativeWiimoteController::Nunchuck;
#endif
	return false;
}

MotionSample EmulatedController::get_second_motion_data() const
{
#ifdef SUPPORTS_WIIMOTE
	std::shared_lock lock(m_mutex);
	if (m_device && m_device->use_motion() && m_device->api() == InputAPI::Wiimote)
	{
		auto* wiimote = (NativeWiimoteController*)m_device.get();
		if (wiimote->get_extension() == NativeWiimoteController::Nunchuck)
			return wiimote->get_nunchuck_motion_sample();
	}
#endif
	return {};
}

bool EmulatedController::has_position() const
{
	std::shared_lock lock(m_mutex);
	return m_device && m_device->has_position();
}

glm::vec2 EmulatedController::get_position() const
{
	std::shared_lock lock(m_mutex);
	if (m_device && m_device->has_position())
		return m_device->get_position();
	return {};
}

glm::vec2 EmulatedController::get_prev_position() const
{
	std::shared_lock lock(m_mutex);
	if (m_device && m_device->has_position())
		return m_device->get_prev_position();
	return {};
}

PositionVisibility EmulatedController::GetPositionVisibility() const
{
	std::shared_lock lock(m_mutex);
	if (m_device && m_device->has_position())
		return m_device->GetPositionVisibility();
	return PositionVisibility::NONE;
}

namespace
{
	// the emulated controller's devices as seen by the binding expressions: the selected device
	// (index 0, the default of unqualified inputs) and the devices bindings name explicitly
	class ControllerDevices : public InputExpr::DeviceSet, public InputExpr::InputSource
	{
	public:
		ControllerDevices(const std::vector<std::shared_ptr<ControllerBase>>& controllers, bool has_device) : m_controllers(controllers), m_has_device(has_device) {}

		size_t default_device() const override { return m_has_device && !m_controllers.empty() ? 0 : InputExpr::kNoDevice; }
		size_t device_count() const override { return m_controllers.size(); }
		std::string device_api(size_t dev) const override { return std::string{InputAPI::to_string(m_controllers[dev]->api())}; }
		std::string device_uuid(size_t dev) const override { return m_controllers[dev]->uuid(); }
		std::string device_display_name(size_t dev) const override { return m_controllers[dev]->display_name(); }
		std::string input_name(size_t dev, InputExpr::InputId id) const override { return m_controllers[dev]->get_button_name(id); }

		std::optional<InputExpr::InputId> find_input(size_t dev, std::string_view name) const override
		{
			// the Keyboard/Mouse device: the mouse's inputs first ("Mouse Left", "TV Cursor X+")
			if (m_controllers[dev]->api() == InputAPI::Keyboard)
			{
				if (const auto mouse = MouseInputs::FromCombinedName(name))
					return *mouse;
			}
			// input ids are small for every API except GTK key values (those are saved as #id)
			constexpr uint64 kScanLimit = 0x200;
			for (uint64 id = 0; id < kScanLimit; ++id)
			{
				if (boost::iequals(m_controllers[dev]->get_button_name(id), name))
					return id;
			}
			return std::nullopt;
		}

		float value(size_t dev, InputExpr::InputId id) const override { return m_controllers[dev]->get_axis_value(id); }
		bool active(size_t dev, InputExpr::InputId id) const override { return m_controllers[dev]->is_input_active(id); }

		size_t index_of(const ControllerBase& controller) const
		{
			for (size_t i = 0; i < m_controllers.size(); ++i)
			{
				if (*m_controllers[i] == controller)
					return i;
			}
			return InputExpr::kNoDevice;
		}

	private:
		const std::vector<std::shared_ptr<ControllerBase>>& m_controllers;
		bool m_has_device;
	};

	// "API/uuid" -> a device instance, for inputs that name a device explicitly
	std::shared_ptr<ControllerBase> create_named_device(std::string_view device_text)
	{
		const auto slash = device_text.find('/');
		if (slash == std::string_view::npos || slash + 1 >= device_text.size())
			return {};
		try
		{
			for (int api = 0; api < InputAPI::MAX; ++api)
			{
				if (!boost::iequals(InputAPI::to_string((InputAPI::Type)api), device_text.substr(0, slash)))
					continue;
				auto device = ControllerFactory::CreateController((InputAPI::Type)api, device_text.substr(slash + 1), "");
				if (device)
					device->connect();
				return device;
			}
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::InputAPI, "input binding names device {}, which can't be created: {}", device_text, ex.what());
		}
		return {};
	}
}

#define DEVICES_LOCKED() ControllerDevices(m_controllers, m_device != nullptr)

void EmulatedController::reparse_mappings_locked()
{
	m_controllers.clear();
	if (m_device)
		m_controllers.emplace_back(m_device);
	m_controllers.insert(m_controllers.end(), m_extra_devices.begin(), m_extra_devices.end());
	const auto devices = DEVICES_LOCKED();
	for (auto& [id, expr] : m_mappings)
		expr = InputExpr::Expression::parse(expr.text(), devices);
}

void EmulatedController::sync_devices_locked()
{
	reparse_mappings_locked();

	// devices named explicitly that aren't attached yet
	bool added = false;
	std::vector<std::string> tried;
	for (const auto& [id, expr] : m_mappings)
	{
		for (const auto& leaf : expr.leaves())
		{
			if (!leaf.qualified || leaf.device != InputExpr::kNoDevice || leaf.device_text.find('/') == std::string::npos)
				continue;
			if (std::find(tried.cbegin(), tried.cend(), leaf.device_text) != tried.cend())
				continue;
			tried.emplace_back(leaf.device_text);
			if (auto device = create_named_device(leaf.device_text))
			{
				m_extra_devices.emplace_back(std::move(device));
				added = true;
			}
		}
	}
	if (added)
		reparse_mappings_locked();

	// extra devices no binding names any more
	const size_t first_extra = m_device ? 1 : 0;
	std::vector<bool> used(m_controllers.size(), false);
	for (const auto& [id, expr] : m_mappings)
	{
		for (const auto& leaf : expr.leaves())
		{
			if (leaf.device != InputExpr::kNoDevice && leaf.device < used.size())
				used[leaf.device] = true;
		}
	}
	bool removed = false;
	for (size_t i = m_extra_devices.size(); i-- > 0;)
	{
		if (!used[first_extra + i])
		{
			m_extra_devices.erase(m_extra_devices.begin() + i);
			removed = true;
		}
	}
	if (removed)
		reparse_mappings_locked();
}

std::shared_ptr<ControllerBase> EmulatedController::get_device() const
{
	std::shared_lock lock(m_mutex);
	return m_device;
}

void EmulatedController::set_device(std::shared_ptr<ControllerBase> device)
{
	if (device)
	{
		device->connect();
#ifdef SUPPORTS_WIIMOTE
		if (const auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(device))
			wiimote->set_player_index(m_player_index);
#endif
	}
	std::scoped_lock lock(m_mutex);
	if (m_device && device && *m_device == *device)
	{
		m_device = std::move(device);
		reparse_mappings_locked();
		return;
	}
	// the old device stays only if a binding names it explicitly
	if (m_device)
		m_extra_devices.emplace_back(m_device);
	if (device)
		std::erase_if(m_extra_devices, [&](const auto& c) { return *c == *device; });
	m_device = std::move(device);
	sync_devices_locked();
}

std::vector<std::shared_ptr<ControllerBase>> EmulatedController::get_extra_devices() const
{
	std::shared_lock lock(m_mutex);
	return m_extra_devices;
}

std::vector<std::shared_ptr<ControllerBase>> EmulatedController::get_controllers() const
{
	std::shared_lock lock(m_mutex);
	return m_controllers;
}

float EmulatedController::get_axis_value(uint64 mapping) const
{
	return std::max(0.0f, get_mapping_value(mapping));
}

float EmulatedController::get_mapping_value(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it == m_mappings.cend())
		return 0;
	return it->second.value(DEVICES_LOCKED());
}

bool EmulatedController::is_mapping_down(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it == m_mappings.cend())
		return false;
	return it->second.active(DEVICES_LOCKED());
}

bool EmulatedController::has_mapping(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	return it != m_mappings.cend() && !it->second.empty();
}

std::string EmulatedController::get_mapping_name(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it == m_mappings.cend())
		return {};
	return it->second.display(DEVICES_LOCKED());
}

bool EmulatedController::is_mapping_explicit(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	return it != m_mappings.cend() && it->second.has_qualified_inputs();
}

std::string EmulatedController::get_mapping_expression(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	return it == m_mappings.cend() ? std::string{} : it->second.text();
}

std::optional<std::string> EmulatedController::set_mapping_expression(uint64 mapping, std::string_view text, bool keep_invalid)
{
	std::scoped_lock lock(m_mutex);
	auto expr = InputExpr::Expression::parse(text, DEVICES_LOCKED());
	if (!expr.ok())
	{
		auto error = expr.error();
		if (keep_invalid)
			m_mappings[mapping] = std::move(expr);
		return error;
	}
	if (expr.empty())
		m_mappings.erase(mapping);
	else
		m_mappings[mapping] = std::move(expr);
	sync_devices_locked();
	return std::nullopt;
}

std::vector<std::string> EmulatedController::get_mapping_warnings(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it == m_mappings.cend())
		return {};
	if (!it->second.ok())
		return {it->second.error()};
	return it->second.warnings();
}

std::optional<uint64> EmulatedController::get_mapping_button(uint64 mapping, const ControllerBase& controller) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it == m_mappings.cend())
		return std::nullopt;
	const size_t index = DEVICES_LOCKED().index_of(controller);
	for (const auto& leaf : it->second.leaves())
	{
		if (leaf.device == index && leaf.input)
			return *leaf.input;
	}
	return std::nullopt;
}

std::vector<uint64> EmulatedController::get_mapping_ids() const
{
	std::shared_lock lock(m_mutex);
	std::vector<uint64> result;
	for (const auto& m : m_mappings)
		result.emplace_back(m.first);
	std::sort(result.begin(), result.end());
	return result;
}

std::vector<std::pair<uint64, uint64>> EmulatedController::get_device_inputs(const ControllerBase& controller) const
{
	std::shared_lock lock(m_mutex);
	std::vector<std::pair<uint64, uint64>> result;
	const size_t index = DEVICES_LOCKED().index_of(controller);
	if (index == InputExpr::kNoDevice)
		return result;
	for (const auto& [mapping, expr] : m_mappings)
	{
		for (const auto& leaf : expr.leaves())
		{
			if (leaf.device == index && leaf.input)
				result.emplace_back(mapping, *leaf.input);
		}
	}
	return result;
}

void EmulatedController::delete_mapping(uint64 mapping)
{
	std::scoped_lock lock(m_mutex);
	m_mappings.erase(mapping);
	sync_devices_locked();
}

void EmulatedController::clear_mappings()
{
	std::scoped_lock lock(m_mutex);
	m_mappings.clear();
	sync_devices_locked();
}

void EmulatedController::add_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller, uint64 button)
{
	std::scoped_lock lock(m_mutex);
	if (DEVICES_LOCKED().index_of(*controller) == InputExpr::kNoDevice)
	{
		// another device (Detect on all devices): attached as an explicitly named device
		m_extra_devices.emplace_back(controller);
		reparse_mappings_locked();
	}
	const auto devices = DEVICES_LOCKED();
	auto& expr = m_mappings[mapping];
	expr = InputExpr::Expression::append(expr, devices.index_of(*controller), button, devices);
	sync_devices_locked();
}

void EmulatedController::replace_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller, uint64 button)
{
	std::scoped_lock lock(m_mutex);
	if (DEVICES_LOCKED().index_of(*controller) == InputExpr::kNoDevice)
	{
		m_extra_devices.emplace_back(controller);
		reparse_mappings_locked();
	}
	const auto devices = DEVICES_LOCKED();
	m_mappings[mapping] = InputExpr::Expression::from_input(devices.index_of(*controller), button, devices);
	sync_devices_locked();
}

InputExpr::Expression EmulatedController::parse_expression(std::string_view text) const
{
	std::shared_lock lock(m_mutex);
	return InputExpr::Expression::parse(text, DEVICES_LOCKED());
}

std::pair<float, bool> EmulatedController::evaluate_expression(const InputExpr::Expression& expression) const
{
	std::shared_lock lock(m_mutex);
	const auto devices = DEVICES_LOCKED();
	return {expression.value(devices), expression.active(devices)};
}

bool EmulatedController::load_default_bindings()
{
	const auto device = get_device();
	std::vector<std::pair<uint64, uint64>> ids;
	std::vector<std::pair<uint64, std::string>> texts;
	if (device)
	{
		ids = get_default_mapping(device);
		if (device->api() == InputAPI::Keyboard)
		{
			// Keyboard/Mouse: the keyboard layout (by key name, so it is the same on every OS) and
			// the mouse's groups
			texts = InputPresets::KeyboardTable(type());
			for (const auto& [control, input] : MouseBindings::MouseDefaults(type_string()))
			{
				if (std::none_of(texts.cbegin(), texts.cend(), [control = control](const auto& t) { return t.first == control; }))
					ids.emplace_back(control, MouseInputs::KeyboardId(input));
			}
		}
	}

	std::scoped_lock lock(m_mutex);
	m_mappings.clear();
	const auto devices = DEVICES_LOCKED();
	for (const auto& [mapping, button] : ids)
	{
		if (std::any_of(texts.cbegin(), texts.cend(), [mapping = mapping](const auto& t) { return t.first == mapping; }))
			continue;
		auto& expr = m_mappings[mapping];
		expr = InputExpr::Expression::append(expr, devices.default_device(), button, devices);
	}
	for (const auto& [mapping, text] : texts)
		m_mappings[mapping] = InputExpr::Expression::parse(text, devices);
	std::erase_if(m_mappings, [](const auto& m) { return m.second.empty(); });
	sync_devices_locked();
	return !m_mappings.empty();
}

bool EmulatedController::operator==(const EmulatedController& o) const
{
	return type() == o.type() && m_player_index == o.m_player_index;
}

bool EmulatedController::operator!=(const EmulatedController& o) const
{
	return !(*this == o);
}
