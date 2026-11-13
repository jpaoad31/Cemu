#pragma once

#include "input/emulated/WPADController.h"
#include "input/emulated/BalanceBoardMath.h"
#include "input/api/Wiimote/WiimoteMessages.h"

// Emulated Wii Balance Board ("balance checker"). It always sits on WPAD channel 3, where Wii
// games (and Wii Fit U) expect it: see InputManager::set_controller.
// The load cell maths come from Dolphin's emulated board (see BalanceBoardMath.h).
//
// Two sources:
// - emulated (default): weight setting, Balance stick or mouse, step off hotkeys;
// - a real board: when a "Wiimote" API controller is assigned to this controller, the game gets
//   that board's raw load cells, temperature and battery and the calibration read from it
//   (WiimoteControllerProvider reads them, see BalanceBoardData). The settings sliders and the
//   step off hotkeys are then unused; Power is the board's A button.
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

	// a real board assigned to this controller (a "Wiimote" API controller)
	struct RealBoard
	{
		bool assigned = false;   // a Wiimote API controller is assigned (the real board is the source)
		bool connected = false;
		bool is_board = false;   // the device identified as a Balance Board
		bool calibrated = false; // its calibration block has been read
		bool crc_ok = false;
		bool has_data = false;   // load cell reports arrive
		size_t device_index = 0;
		uint8 status_battery = 0; // status report battery byte
		BalanceBoardData data{};

		bool ready() const { return connected && is_board && calibrated && has_data; }
	};
	RealBoard get_real_board() const;
	bool uses_real_board() const;
	// a real board is assigned but its calibration hasn't been read yet (WBC calibration waits)
	bool is_real_calibration_pending() const;
	// forwards a WPADControlBLC command to the real board, if one is the source
	void send_real_command(uint8 command);

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

	// the board's extension memory (calibration block, CRC, temperature, ID): the real board's
	// when one is assigned and has been read, the emulated one otherwise
	BalanceBoardMath::ExtMemory get_extension_memory() const;
	// battery byte of WPADBLStatus (the real board's extension byte, or the Battery setting)
	uint8 get_battery_raw() const;
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

	// last calibration and sample of the real board, kept while it is reconnecting
	mutable std::mutex m_real_mutex;
	mutable std::optional<BalanceBoardMath::ExtMemory> m_real_memory;
	mutable std::optional<BalanceBoardData> m_real_last;
	mutable int m_real_logged_state = -1;
};
