#pragma once
#include <pugixml.hpp>

class ControllerBase;

#include "input/motion/MotionSample.h"

#include "input/api/ControllerState.h"
#include "input/api/InputAPI.h"
#include "input/emulated/InputExpression.h"

#include "util/helpers/helpers.h"

// mapping = wii u controller button id
// button = api button id

class EmulatedController
{
	friend class InputManager;
public:
	EmulatedController(size_t player_index);
	virtual ~EmulatedController() = default;

	virtual void load(const pugi::xml_node& node){};
	virtual void save(pugi::xml_node& node){};

	enum Type
	{
		VPAD,
		Pro,
		Classic,
		Wiimote,
		BalanceBoard,

		MAX
	};
	virtual Type type() const = 0;
	std::string_view type_string() const { return type_to_string(type()); }

	static std::string_view type_to_string(Type type);
	static Type type_from_string(std::string_view str);

	size_t player_index() const { return m_player_index; }
	const std::string& get_profile_name() const { return m_profile_name; }
	bool has_profile_name() const { return !m_profile_name.empty() && m_profile_name != "default"; }

	void calibrate();

	void connect();
	virtual void update();
	void controllers_update_states();
	void controllers_read_done();

	virtual glm::vec2 get_axis() const = 0;
	virtual glm::vec2 get_rotation() const = 0;
	virtual glm::vec2 get_trigger() const = 0;

	void start_rumble();
	void stop_rumble();

	bool is_battery_low() const;

	bool has_motion() const;
	MotionSample get_motion_data() const;

	// some controllers (nunchuck) provide extra motion data
	bool has_second_motion() const;
	MotionSample get_second_motion_data() const;

	bool has_position() const;
	glm::vec2 get_position() const;
	glm::vec2 get_prev_position() const;
	PositionVisibility GetPositionVisibility() const;

	// One device per emulated controller (Dolphin's default device, see INPUT_REWORK.md): plain
	// bindings are its inputs, Detect listens to it, and its own data (motion, touch, the Wii
	// Remote's IR, a real board's sensors, rumble, battery) is this controller's. nullptr: none.
	std::shared_ptr<ControllerBase> get_device() const;
	// selects a device; the bindings stay as they are written (`X` now means the new device's X)
	void set_device(std::shared_ptr<ControllerBase> device);
	// devices that bindings name explicitly (`API/uuid:Input`, advanced): attached on demand so
	// those inputs evaluate, dropped when no binding names them any more
	std::vector<std::shared_ptr<ControllerBase>> get_extra_devices() const;
	// the device and the extra devices (to update their state)
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() const;

	// Bindings (see InputExpression.h and INPUT_REWORK.md): every emulated control maps to an
	// expression, usually a list of the device's inputs OR'ed together.
	bool is_mapping_down(uint64 mapping) const;
	float get_mapping_value(uint64 mapping) const; // 0..1 (axis directions: the deflection)
	bool has_mapping(uint64 mapping) const;
	// short text for the UI ("X | A [Remote GamePad]": inputs of another device are tagged)
	std::string get_mapping_name(uint64 mapping) const;
	// the binding names another device explicitly (advanced)
	bool is_mapping_explicit(uint64 mapping) const;
	// canonical expression text, as saved in the profile
	std::string get_mapping_expression(uint64 mapping) const;
	// replaces the binding with an expression; returns the parse error, if any (then nothing changes,
	// unless keep_invalid: profile loading keeps the text so it isn't lost, the binding stays inactive)
	std::optional<std::string> set_mapping_expression(uint64 mapping, std::string_view text, bool keep_invalid = false);
	// warnings for a binding: inputs of devices that aren't attached, unknown input names
	std::vector<std::string> get_mapping_warnings(uint64 mapping) const;
	// the binding's input on this device (the first one), for the old profile format
	std::optional<uint64> get_mapping_button(uint64 mapping, const ControllerBase& controller) const;
	std::vector<uint64> get_mapping_ids() const;
	// every (control, input) pair that uses this device
	std::vector<std::pair<uint64, uint64>> get_device_inputs(const ControllerBase& controller) const;
	void delete_mapping(uint64 mapping);
	void clear_mappings();
	// adds an input to the control's list (nothing if it's there already). An input of another
	// device than the selected one is written with its device (explicit) and attaches it.
	void add_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller_base, uint64 button);
	// the control maps to this one input only
	void replace_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller_base, uint64 button);

	// an expression over this controller's devices, for the UI's expression editor
	InputExpr::Expression parse_expression(std::string_view text) const;
	std::pair<float, bool> evaluate_expression(const InputExpr::Expression& expression) const;

	virtual uint32 get_emulated_button_flag(uint32 mapping) const = 0;

	bool operator==(const EmulatedController& o) const;
	bool operator!=(const EmulatedController& o) const;

	virtual size_t get_highest_mapping_id() const = 0;
	virtual bool is_axis_mapping(uint64 mapping) const = 0;

	virtual bool is_start_down() const = 0;
	virtual bool is_left_down() const = 0;
	virtual bool is_right_down() const = 0;
	virtual bool is_up_down() const = 0;
	virtual bool is_down_down() const = 0;
	virtual bool is_a_down() const = 0;
	virtual bool is_b_down() const = 0;
	virtual bool is_home_down() const = 0;

	bool was_home_button_down() { return std::exchange(m_homebutton_down, false); }

	// Default bindings of a device: (emulated control, device input) pairs
	virtual std::vector<std::pair<uint64, uint64>> get_default_mapping(const std::shared_ptr<ControllerBase>& controller) const { return {}; }
	// Replaces every binding with the selected device's defaults (the Keyboard/Mouse device: the
	// keyboard layout of InputPresets::KeyboardTable and the mouse's touch/tilt/pointer/balance).
	// Returns false when the device has none (the bindings are cleared then).
	bool load_default_bindings();

protected:
	size_t m_player_index;
	std::string m_profile_name = "default";

	mutable std::shared_mutex m_mutex;
	std::shared_ptr<ControllerBase> m_device;
	std::vector<std::shared_ptr<ControllerBase>> m_extra_devices;
	// m_device (if any) first, then m_extra_devices: the expressions' device indices. Guarded by m_mutex.
	std::vector<std::shared_ptr<ControllerBase>> m_controllers;

	float get_axis_value(uint64 mapping) const;
	bool m_rumble = false;

	// emulated control -> binding. Guarded by m_mutex. Re-parsed whenever the device list changes
	// (the leaves refer to devices by index).
	std::unordered_map<uint64, InputExpr::Expression> m_mappings;
	void reparse_mappings_locked();
	// attaches the devices bindings name explicitly, drops extra devices nothing names, re-parses
	void sync_devices_locked();
	std::shared_ptr<ControllerBase> device_locked() const { return m_device; }

	bool m_homebutton_down = false;
};

using EmulatedControllerPtr = std::shared_ptr<EmulatedController>;

template <>
struct fmt::formatter<EmulatedController::Type> : formatter<string_view> {
	template <typename FormatContext>
	auto format(EmulatedController::Type v, FormatContext& ctx) const {
		switch (v)
		{
		case EmulatedController::Type::VPAD: return formatter<string_view>::format("Wii U Gamepad", ctx);
		case EmulatedController::Type::Pro: return formatter<string_view>::format("Wii U Pro Controller", ctx);
		case EmulatedController::Type::Classic: return formatter<string_view>::format("Wii U Classic Controller Pro", ctx);
		case EmulatedController::Type::Wiimote: return formatter<string_view>::format("Wiimote", ctx);
		case EmulatedController::Type::BalanceBoard: return formatter<string_view>::format("Balance Board", ctx);
		}
		throw std::invalid_argument(fmt::format("invalid emulated controller type with value {}", stdx::to_underlying(v)));
	}
};
