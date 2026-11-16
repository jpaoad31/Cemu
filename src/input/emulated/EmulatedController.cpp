#include "input/emulated/EmulatedController.h"

#include "input/api/Controller.h"

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

void EmulatedController::start_rumble()
{
	m_rumble = true;
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->start_rumble();
	}
}

void EmulatedController::stop_rumble()
{
	if (!m_rumble)
		return;

	m_rumble = false;

	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		controller->stop_rumble();
	}
}

bool EmulatedController::is_battery_low() const
{
	std::shared_lock lock(m_mutex);
	return std::any_of(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) {return c->has_low_battery(); });
}

bool EmulatedController::has_motion() const
{
	std::shared_lock lock(m_mutex);
	return std::any_of(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) {return c->use_motion(); });
}

MotionSample EmulatedController::get_motion_data() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->use_motion())
			return controller->get_motion_sample();
	}

	return {};
}

bool EmulatedController::has_second_motion() const
{
	int motion = 0;
	std::shared_lock lock(m_mutex);
	for(const auto& controller : m_controllers)
	{
		if(controller->use_motion())
		{
			// if wiimote has nunchuck connected, we use its acceleration
            #if SUPPORTS_WIIMOTE
            if(controller->api() == InputAPI::Wiimote)
			{
				if(((NativeWiimoteController*)controller.get())->get_extension() == NativeWiimoteController::Nunchuck)
				{
					return true;
				}
			}
            #endif
			motion++;
		}
	}

	return motion >= 2;
}

MotionSample EmulatedController::get_second_motion_data() const
{
	int motion = 0;
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->use_motion())
		{
			// if wiimote has nunchuck connected, we use its acceleration
            #ifdef SUPPORTS_WIIMOTE
			if (controller->api() == InputAPI::Wiimote)
			{
				if (((NativeWiimoteController*)controller.get())->get_extension() == NativeWiimoteController::Nunchuck)
				{
					return ((NativeWiimoteController*)controller.get())->get_nunchuck_motion_sample();
				}
			}
			#endif

			motion++;
			if(motion == 2)
			{
				return controller->get_motion_sample();
			}
		}
	}

	return {};
}

bool EmulatedController::has_position() const
{
	std::shared_lock lock(m_mutex);
	return std::any_of(m_controllers.cbegin(), m_controllers.cend(), [](const auto& c) {return c->has_position(); });
}

glm::vec2 EmulatedController::get_position() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->has_position())
			return controller->get_position();
	}

	return {};
}

glm::vec2 EmulatedController::get_prev_position() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->has_position())
			return controller->get_prev_position();
	}

	return {};
}

PositionVisibility EmulatedController::GetPositionVisibility() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& controller : m_controllers)
	{
		if (controller->has_position())
			return controller->GetPositionVisibility();
	}
	return PositionVisibility::NONE;
}

namespace
{
	// the emulated controller's device list as seen by the binding expressions
	class ControllerDevices : public InputExpr::DeviceSet, public InputExpr::InputSource
	{
	public:
		explicit ControllerDevices(const std::vector<std::shared_ptr<ControllerBase>>& controllers) : m_controllers(controllers) {}

		size_t device_count() const override { return m_controllers.size(); }
		std::string device_api(size_t dev) const override { return std::string{InputAPI::to_string(m_controllers[dev]->api())}; }
		std::string device_uuid(size_t dev) const override { return m_controllers[dev]->uuid(); }
		std::string device_display_name(size_t dev) const override { return m_controllers[dev]->display_name(); }
		std::string input_name(size_t dev, InputExpr::InputId id) const override { return m_controllers[dev]->get_button_name(id); }

		std::optional<InputExpr::InputId> find_input(size_t dev, std::string_view name) const override
		{
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
		bool active(size_t dev, InputExpr::InputId id) const override { return m_controllers[dev]->get_state().buttons.GetButtonState((uint32)id); }

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
	};
}

void EmulatedController::reparse_mappings_locked()
{
	const ControllerDevices devices(m_controllers);
	for (auto& [id, expr] : m_mappings)
		expr = InputExpr::Expression::parse(expr.text(), devices);
}

void EmulatedController::add_controller(std::shared_ptr<ControllerBase> controller)
{
	controller->connect();

    #ifdef SUPPORTS_WIIMOTE
    if (const auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(controller)) {
		wiimote->set_player_index(m_player_index);
	}
    #endif
	std::scoped_lock lock(m_mutex);
	m_controllers.emplace_back(std::move(controller));
	// bindings that named this device before it was attached now resolve
	reparse_mappings_locked();
}

void EmulatedController::remove_controller(const std::shared_ptr<ControllerBase>& controller)
{
	std::scoped_lock lock(m_mutex);
	const auto it = std::find_if(m_controllers.cbegin(), m_controllers.cend(), [&](const auto& c) { return *c == *controller; });
	if (it != m_controllers.cend())
	{
		// drop the device's inputs from the binding lists (expressions keep them, unresolved)
		{
			const ControllerDevices devices(m_controllers);
			const size_t index = it - m_controllers.cbegin();
			for (auto m = m_mappings.begin(); m != m_mappings.end();)
			{
				m->second = InputExpr::Expression::remove_device(m->second, index, devices);
				if (m->second.empty())
					m = m_mappings.erase(m);
				else
					++m;
			}
		}
		m_controllers.erase(it);
		reparse_mappings_locked();
	}
}

void EmulatedController::clear_controllers()
{
	std::scoped_lock lock(m_mutex);
	m_controllers.clear();
	reparse_mappings_locked();
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
	return it->second.value(ControllerDevices(m_controllers));
}

bool EmulatedController::is_mapping_down(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it == m_mappings.cend())
		return false;
	return it->second.active(ControllerDevices(m_controllers));
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
	return it->second.display(ControllerDevices(m_controllers), m_controllers.size() > 1);
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
	auto expr = InputExpr::Expression::parse(text, ControllerDevices(m_controllers));
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

std::shared_ptr<ControllerBase> EmulatedController::get_mapping_controller(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it == m_mappings.cend())
		return {};
	for (const auto& leaf : it->second.leaves())
	{
		if (leaf.device < m_controllers.size())
			return m_controllers[leaf.device];
	}
	return {};
}

std::optional<uint64> EmulatedController::get_mapping_button(uint64 mapping, const ControllerBase& controller) const
{
	std::shared_lock lock(m_mutex);
	const auto it = m_mappings.find(mapping);
	if (it == m_mappings.cend())
		return std::nullopt;
	const size_t index = ControllerDevices(m_controllers).index_of(controller);
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
	const size_t index = ControllerDevices(m_controllers).index_of(controller);
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
}

void EmulatedController::clear_mappings()
{
	std::scoped_lock lock(m_mutex);
	m_mappings.clear();
}

void EmulatedController::set_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller,
                                     uint64 button)
{
	std::scoped_lock lock(m_mutex);
	const ControllerDevices devices(m_controllers);
	const size_t index = devices.index_of(*controller);
	if (index == InputExpr::kNoDevice)
		return;
	auto& expr = m_mappings[mapping];
	expr = InputExpr::Expression::append(InputExpr::Expression::remove_device(expr, index, devices), index, button, devices);
}

void EmulatedController::add_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller, uint64 button)
{
	std::scoped_lock lock(m_mutex);
	const ControllerDevices devices(m_controllers);
	const size_t index = devices.index_of(*controller);
	if (index == InputExpr::kNoDevice)
		return;
	auto& expr = m_mappings[mapping];
	expr = InputExpr::Expression::append(expr, index, button, devices);
}

void EmulatedController::replace_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller, uint64 button)
{
	std::scoped_lock lock(m_mutex);
	const ControllerDevices devices(m_controllers);
	const size_t index = devices.index_of(*controller);
	if (index == InputExpr::kNoDevice)
		return;
	m_mappings[mapping] = InputExpr::Expression::from_input(index, button, devices);
}

void EmulatedController::remove_device_mappings(const std::shared_ptr<ControllerBase>& controller)
{
	std::scoped_lock lock(m_mutex);
	const ControllerDevices devices(m_controllers);
	const size_t index = devices.index_of(*controller);
	if (index == InputExpr::kNoDevice)
		return;
	for (auto m = m_mappings.begin(); m != m_mappings.end();)
	{
		m->second = InputExpr::Expression::remove_device(m->second, index, devices);
		if (m->second.empty())
			m = m_mappings.erase(m);
		else
			++m;
	}
}

InputExpr::Expression EmulatedController::parse_expression(std::string_view text) const
{
	std::shared_lock lock(m_mutex);
	return InputExpr::Expression::parse(text, ControllerDevices(m_controllers));
}

std::pair<float, bool> EmulatedController::evaluate_expression(const InputExpr::Expression& expression) const
{
	std::shared_lock lock(m_mutex);
	const ControllerDevices devices(m_controllers);
	return {expression.value(devices), expression.active(devices)};
}

bool EmulatedController::set_default_mapping(const std::shared_ptr<ControllerBase>& controller, bool replace)
{
	const auto defaults = get_default_mapping(controller);
	std::scoped_lock lock(m_mutex);
	const ControllerDevices devices(m_controllers);
	const size_t index = devices.index_of(*controller);
	if (index == InputExpr::kNoDevice)
		return false;

	bool changed = false;
	if (replace)
	{
		for (auto m = m_mappings.begin(); m != m_mappings.end();)
		{
			auto updated = InputExpr::Expression::remove_device(m->second, index, devices);
			changed |= updated.text() != m->second.text();
			m->second = std::move(updated);
			if (m->second.empty())
				m = m_mappings.erase(m);
			else
				++m;
		}
	}

	for (const auto& [mapping, button] : defaults)
	{
		auto& expr = m_mappings[mapping];
		// never steal: a device only adds to the controls where it has nothing yet
		if (expr.references_device(index))
			continue;
		expr = InputExpr::Expression::append(expr, index, button, devices);
		changed = true;
	}
	for (auto m = m_mappings.begin(); m != m_mappings.end();)
	{
		if (m->second.empty())
			m = m_mappings.erase(m);
		else
			++m;
	}
	return changed;
}

bool EmulatedController::operator==(const EmulatedController& o) const
{
	return type() == o.type() && m_player_index == o.m_player_index;
}

bool EmulatedController::operator!=(const EmulatedController& o) const
{
	return !(*this == o);
}
