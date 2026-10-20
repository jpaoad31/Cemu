#pragma once

#include "input/emulated/WPADController.h"
#include "input/emulated/BalanceBoardMath.h"

// Emulated Wii Balance Board ("balance checker"). It always sits on WPAD channel 3, where Wii
// games (and Wii Fit U) expect it: see InputManager::set_controller.
// The load cell maths come from Dolphin's emulated board (see BalanceBoardMath.h).
class BalanceBoardController : public WPADController
{
	using base_type = WPADController;
public:
	// WPAD channel the board is attached to
	static constexpr size_t kWPADChannel = 3;

	enum ButtonId
	{
		kButtonId_None,

		kButtonId_Power,

		kButtonId_Balance_Up,
		kButtonId_Balance_Down,
		kButtonId_Balance_Left,
		kButtonId_Balance_Right,

		kButtonId_StepOffToggle,
		kButtonId_StepOffHold,

		kButtonId_Max,
	};

	BalanceBoardController(size_t player_index);

	Type type() const override { return Type::BalanceBoard; }
	WPADDeviceType get_device_type() const override { return kWAPDevBalanceChecker; }

	uint32 get_emulated_button_flag(uint32 id) const override;
	size_t get_highest_mapping_id() const override { return kButtonId_Max; }
	bool is_axis_mapping(uint64 mapping) const override { return mapping >= kButtonId_Balance_Up && mapping <= kButtonId_Balance_Right; }

	bool set_default_mapping(const std::shared_ptr<ControllerBase>& controller) override;

	// centre of balance from the Balance stick (x: right, y: front), or from the mouse when the
	// stick is unmapped
	glm::vec2 get_axis() const override;
	bool is_balance_mapped() const;
	static glm::vec2 get_mouse_axis();
	glm::vec2 get_rotation() const override { return {}; }
	glm::vec2 get_trigger() const override { return {}; }

	void load(const pugi::xml_node& node) override;
	void save(pugi::xml_node& node) override;

	static std::string_view get_button_name(ButtonId id);

	// the board only has the power button (reported as A)
	bool is_start_down() const override { return false; }
	bool is_left_down() const override { return false; }
	bool is_right_down() const override { return false; }
	bool is_up_down() const override { return false; }
	bool is_down_down() const override { return false; }
	bool is_a_down() const override { return is_mapping_down(kButtonId_Power); }
	bool is_b_down() const override { return false; }
	bool is_home_down() const override { return false; }

	// fills press[], temp and battery of a WPADBLStatus (button/dev/err are set by WPADRead)
	void fill_balance_status(WPADBLStatus_t& status) override;

	// current raw load cell values (TR, BR, TL, BL), updates the step off state
	std::array<uint16, BalanceBoardMath::kSensorCount> get_sensor_values();
	bool is_stepped_off() const { return m_step_off_toggled.load() != m_step_off_hold.load(); }
	// raw values for a given state, without polling the step off keys
	std::array<uint16, BalanceBoardMath::kSensorCount> compute_sensor_values(bool stepped_off) const;

	// the board's extension memory (calibration block, CRC, temperature, ID)
	const BalanceBoardMath::ExtMemory& get_extension_memory() const { return m_ext_memory; }
	uint8 get_battery_raw() const { return BalanceBoardMath::BatteryPercentToRaw(m_battery); }
	int get_battery_level() const { return BalanceBoardMath::BatteryRawToLevel(get_battery_raw()); }

	// settings
	double get_weight() const { return m_weight; }
	void set_weight(double kg) { m_weight = std::clamp(kg, 0.0, BalanceBoardMath::kMaxWeight); }
	double get_foot_separation() const { return m_foot_separation; }
	void set_foot_separation(double cm) { m_foot_separation = std::clamp(cm, 0.0, 50.0); }
	double get_foot_length() const { return m_foot_length; }
	void set_foot_length(double cm) { m_foot_length = std::clamp(cm, 0.0, 40.0); }
	int get_battery() const { return m_battery; }
	void set_battery(int percent) { m_battery = std::clamp(percent, 0, 100); }

private:
	void update_step_off();

	double m_weight = BalanceBoardMath::kDefaultWeight;
	double m_foot_separation = BalanceBoardMath::kDefaultFootSeparation;
	double m_foot_length = BalanceBoardMath::kDefaultFootLength;
	int m_battery = BalanceBoardMath::kDefaultBattery;

	// polled from the emulation thread and the input settings window
	std::atomic<bool> m_step_off_toggled = false;
	std::atomic<bool> m_step_off_hold = false;
	std::atomic<bool> m_step_off_toggle_was_down = false;

	BalanceBoardMath::ExtMemory m_ext_memory;
};
