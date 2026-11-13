#pragma once

#include "input/api/Controller.h"
#include "input/api/Wiimote/WiimoteControllerProvider.h"

// todo: find better name because of emulated nameclash
class NativeWiimoteController : public Controller<WiimoteControllerProvider>
{
public:
	NativeWiimoteController(size_t index);

	enum Extension
	{
		None,
		Nunchuck,
		Classic,
		MotionPlus,
	};
	
	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::Wiimote) == "Wiimote");
		return to_string(InputAPI::Wiimote);
	}
	InputAPI::Type api() const override { return InputAPI::Wiimote; }

	void save(pugi::xml_node& node) override;
	void load(const pugi::xml_node& node) override;

	bool connect() override;
	bool is_connected() override;

	void set_player_index(size_t player_index);

	Extension get_extension() const;
	bool is_mpls_attached() const;

	// real Wii Balance Board (identified by its extension ID or HID name)
	bool is_balance_board() const;
	// load cells, temperature, battery and the calibration read from the board
	std::optional<BalanceBoardData> get_balance_board() const;
	// battery byte of the last status report
	uint8 get_battery_level() const;
	size_t get_index() const { return m_index; }
	void send_balance_board_command(uint8 command) { m_provider->send_balance_board_command(m_index, command); }

	ControllerState raw_state() override;

	bool has_position() override;
	glm::vec2 get_position() override;
	glm::vec2 get_prev_position() override;
	PositionVisibility GetPositionVisibility() override;
	
	bool has_motion() override { return true; }
	bool has_rumble() override { return true; }

	bool has_battery() override { return true; }
	bool has_low_battery() override;
	
	void start_rumble() override;
	void stop_rumble() override;

	MotionSample get_motion_sample() override;
	MotionSample get_nunchuck_motion_sample() const;

	std::string get_button_name(uint64 button) const override;

	uint32 get_packet_delay();
	void set_packet_delay(uint32 delay);

private:
	size_t m_index;
	size_t m_player_index = 0;
	uint32 m_packet_delay = WiimoteControllerProvider::kDefaultPacketDelay;
};

