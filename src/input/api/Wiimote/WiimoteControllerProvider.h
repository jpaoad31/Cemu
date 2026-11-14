#pragma once

#include "input/motion/MotionHandler.h"
#include "input/api/Wiimote/WiimoteDevice.h"
#include "input/api/Wiimote/WiimoteMessages.h"
#include "input/emulated/WiimoteSpeakerCodec.h"

#include "input/api/ControllerProvider.h"
#include "input/api/ControllerState.h"

#include <deque>
#include <list>
#include <unordered_map>
#include <variant>
#include <boost/ptr_container/ptr_vector.hpp>

#ifndef HAS_WIIMOTE
#define HAS_WIIMOTE 1
#endif

#define WIIMOTE_DEBUG 1

class WiimoteControllerProvider : public ControllerProviderBase
{
	friend class WiimoteController;
public:
	constexpr static uint32 kDefaultPacketDelay = 25;

	WiimoteControllerProvider();
	~WiimoteControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::Wiimote;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	bool is_connected(size_t index);
	// changes whenever another connection takes the index (a remote that reconnected needs its
	// speaker set up again); 0 = nothing connected
	uint32 get_connection_id(size_t index);
	bool is_registered_device(size_t index);
	void set_rumble(size_t index, bool state);
	void request_status(size_t index);
	void set_led(size_t index, size_t player_index);

	uint32 get_packet_delay(size_t index);
	void set_packet_delay(size_t index, uint32 delay);

	struct WiimoteState
	{
		uint16 buttons = 0;
		uint8 flags = 0;
		uint8 battery_level = 0;

		glm::vec3 m_acceleration{}, m_prev_acceleration{};
		float m_roll = 0;

		std::chrono::high_resolution_clock::time_point m_last_motion_timestamp{};
		MotionSample motion_sample{};
		WiiUMotionHandler motion_handler{};

		bool m_calibrated = false;
		Calibration m_calib_acceleration{};

		struct IRCamera
		{
			IRMode mode = kIRDisabled;
			std::array<IRDot, 4> dots{}, prev_dots{};

			glm::vec2 position{}, m_prev_position{};
			PositionVisibility m_positionVisibility;
			glm::vec2 middle {};
			float distance = 0;
			std::pair<sint32, sint32> indices{ 0,1 };
		}ir_camera{};

		std::optional<MotionPlusData> m_motion_plus;
		std::variant<std::monostate, NunchuckData, ClassicData, BalanceBoardData> m_extension{};

		// the HID name says Balance Board ("Nintendo RVL-WBC"); confirmed by the extension ID
		bool balance_board_name = false;
		bool is_balance_board() const { return balance_board_name || std::holds_alternative<BalanceBoardData>(m_extension); }
	};
	WiimoteState get_state(size_t index);
	// the Balance Board part of the state, if this device is a Balance Board (cheaper than get_state)
	std::optional<BalanceBoardData> get_balance_board(size_t index);
	bool is_balance_board(size_t index);
	uint8 get_battery_level(size_t index);
	// forwards a game's WPADControlBLC command (0x55 enable, 0xAA update, 0x00 temperature) to a real board
	void send_balance_board_command(size_t index, uint8 command);

	// Speaker. The setup and stream reports of a remote go through their own queue, in order and
	// paced at the stream's rate (one report per WiimoteSpeakerCodec::Config::ReportInterval), so
	// a burst from the game doesn't overflow the remote's small buffer.
	// on: enable, mute, configure (registers 0xA20009, 0xA20001), unmute
	void speaker_on(size_t index, const WiimoteSpeakerCodec::Config& config);
	// off: mute, reset the registers, disable
	void speaker_off(size_t index);
	void speaker_mute(size_t index, bool mute);
	// play: 0x01 to 0xA20008, then the stream reports
	void speaker_play(size_t index);
	// room for another stream report (false while kSpeakerQueueMax are waiting)
	bool speaker_can_send(size_t index);
	// queues one 0x18 report with up to 20 bytes; false (dropped) if the queue is full
	bool speaker_send(size_t index, const uint8* data, size_t length);
	static constexpr size_t kSpeakerQueueMax = 8; // ~53 ms of 6 kHz ADPCM

private:
	std::atomic_bool m_running = false;
	std::thread m_reader_thread, m_writer_thread;
	std::shared_mutex m_device_mutex;

	std::thread m_connectionThread;
	std::mutex m_connectionMutex;
    std::condition_variable m_connectionCond;
	std::vector<WiimoteDevicePtr> m_connectedDevices;
	std::mutex m_connectedDeviceMutex;
	struct Wiimote
	{
		Wiimote(WiimoteDevicePtr device)
			: device(std::move(device)) {}

		WiimoteDevicePtr device;
		uint32 connection_id = 0; // unique per connection (see get_connection_id)
		std::atomic_bool rumble = false;

		std::shared_mutex mutex;
		WiimoteState state{};

		std::atomic_uint32_t data_delay = kDefaultPacketDelay;
		std::atomic_uint8_t report_mode = 0; // last data report mode sent, for logging
		std::chrono::high_resolution_clock::time_point data_ts{};
	};
	boost::ptr_vector<Wiimote> m_wiimotes;
	// the device that last used each index (WiimoteDevice::get_identity()): a device that comes back
	// gets its old index again, so controller profiles (which refer to Wiimotes by index) still point
	// at the same device even when several come back in a different order
	std::vector<std::string> m_index_owner;
	uint32 m_next_connection_id = 1;

	std::list<std::pair<size_t,std::vector<uint8>>> m_write_queue;
	std::mutex m_writer_mutex;
	std::condition_variable m_writer_cond;

	// speaker reports per device index (guarded by m_writer_mutex), see speaker_on
	struct SpeakerStream
	{
		struct Report
		{
			std::vector<uint8> data;
			bool stream; // 0x18 data (counted against kSpeakerQueueMax)
		};
		std::deque<Report> queue;
		size_t stream_queued = 0;
		WiimoteSpeakerCodec::StreamPacer pacer;
		// statistics, logged every kSpeakerStatsReports
		uint64 sent = 0, dropped = 0;
		uint64 window_sent = 0;
		std::chrono::steady_clock::time_point window_start{};
	};
	std::unordered_map<size_t, SpeakerStream> m_speaker;
	size_t m_speaker_queued = 0;
	void speaker_queue(size_t index, std::vector<uint8> report, bool stream);
	struct DueSpeakerReport
	{
		size_t index;
		std::vector<uint8> data;
		bool stream;
	};
	void writer_take_speaker_reports(std::vector<DueSpeakerReport>& due);
	void writer_send_speaker_reports(std::vector<DueSpeakerReport>& due);

	void reader_thread();
	void writer_thread();
	void connectionThread();

	void calibrate(size_t index);
	IRMode set_ir_camera(size_t index, bool state);

	void send_packet(size_t index, std::vector<uint8> data);
	void send_read_packet(size_t index, MemoryType type, RegisterAddress address, uint16 size);
	void send_write_packet(size_t index, MemoryType type, RegisterAddress address, const std::vector<uint8>& data);

	void parse_acceleration(WiimoteState& wiimote_state, const uint8*& data);

	void rotate_ir(WiimoteState& wiimote_state);
	void calculate_ir_position(WiimoteState& wiimote_state);
	int parse_ir(WiimoteState& wiimote_state, const uint8* data);

	void request_extension(size_t index);
	void request_balance_board_calibration(size_t index);
	void parse_balance_board(size_t index, WiimoteState& state, const uint8* data, size_t size);
	void detect_motion_plus(size_t index);
	void set_motion_plus(size_t index, bool state);

	void update_report_type(size_t index);
};



