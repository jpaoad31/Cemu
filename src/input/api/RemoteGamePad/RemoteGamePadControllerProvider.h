#pragma once

#include "input/api/ControllerProvider.h"
#include "input/api/RemoteGamePad/RemoteGamePadProtocol.h"
#include "input/api/RemoteGamePad/RemoteGamePadTransport.h"
#include "input/motion/MotionHandler.h"

#include <deque>

#ifndef HAS_REMOTE_GAMEPAD
#define HAS_REMOTE_GAMEPAD 1
#endif

// "Remote GamePad": an iPhone app (jpaoad31/wiiu-gamepad-remote) sends GamePad buttons, sticks,
// touch and motion (UDP now; the receive path goes through RemoteGamePadTransport so BLE can be
// added later). See REMOTE_GAMEPAD.md and the app's PROTOCOL.md.

struct RemoteGamePadProviderSettings : public ControllerProviderSettings
{
	uint16 port = RemoteGamePad::kDefaultPort;

	RemoteGamePadProviderSettings() = default;
	explicit RemoteGamePadProviderSettings(uint16 port) : port(port) {}

	bool operator==(const RemoteGamePadProviderSettings& s) const { return port == s.port; }

	bool operator==(const ControllerProviderSettings& s) const override
	{
		const auto* ptr = dynamic_cast<const RemoteGamePadProviderSettings*>(&s);
		return ptr && *this == *ptr;
	}
};

class RemoteGamePadControllerProvider : public ControllerProvider<RemoteGamePadProviderSettings>
{
	friend class RemoteGamePadController;
	using base_type = ControllerProvider<RemoteGamePadProviderSettings>;

public:
	struct State
	{
		bool connected = false;
		RemoteGamePad::Input input{};
		MotionSample motion{};
		// presses not yet seen by a game read (VPADRead): reported as down to the next read even if
		// already released, so a tap shorter than a game frame isn't lost
		uint32 latched_buttons = 0;
		bool latched_touch = false;
		float latched_touch_x = 0, latched_touch_y = 0;
		uint32 touch_latch_seq = 0;
	};

	struct Status
	{
		uint16 port = 0;
		bool listening = false;
		std::string error;
		bool connected = false;
		std::string phone_name, phone_model, app_version, address;
		std::string transport; // "UDP port 26770"
		uint16 capabilities = 0; // the phone's, from HELLO
		float packet_rate = 0;
		float max_gap_ms = 0; // longest interval between input packets, last ~3 s
		uint64 packets = 0;
		// live: what the newest packet holds
		uint32 buttons = 0;
		bool touch = false;
		float touch_x = 0, touch_y = 0;
	};

	RemoteGamePadControllerProvider();
	RemoteGamePadControllerProvider(const RemoteGamePadProviderSettings& settings);
	~RemoteGamePadControllerProvider() override;

	inline static InputAPI::Type kAPIType = InputAPI::RemoteGamePad;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	// opens the UDP port (once); called when a controller uses this provider
	void start();

	bool is_connected() const;
	State get_state() const;
	// a game read has reported these latched presses: drop them (touch only if no newer touch-down)
	void consume_latch(uint32 buttons, bool touch, uint32 touch_latch_seq);
	Status get_status() const;
	std::string get_status_text() const;

private:
	void reader_thread();
	void handle_packet(const uint8_t* data, size_t size, const RemoteGamePadTransport::Peer& sender);
	void send_hello_ack(uint32 client_id, const RemoteGamePadTransport::Peer& to);
	void send_input_ack();
	bool MediaPortHere() const;

	std::mutex m_start_mutex;
	std::atomic_bool m_running = false;
	std::thread m_reader_thread;
	// only the reader thread uses the transport after start()
	std::unique_ptr<RemoteGamePadTransport> m_transport;

	mutable std::mutex m_mutex;
	// protected by m_mutex
	State m_state{};
	Status m_status{};
	std::chrono::steady_clock::time_point m_last_input{};
	RemoteGamePadTransport::Peer m_client{};
	uint32 m_client_id = 0;
	bool m_have_seq = false;
	uint32 m_last_seq = 0;
	uint64 m_last_motion_ts = 0;
	WiiUMotionHandler m_motion_handler{};
	RemoteGamePad::PressLatch m_latch; // see State
	// packet rate and gaps
	std::chrono::steady_clock::time_point m_rate_window_start{};
	uint32 m_rate_count = 0;
	std::deque<std::pair<std::chrono::steady_clock::time_point, float>> m_gaps; // (arrival, ms since previous)
	// latency echo
	std::chrono::steady_clock::time_point m_last_ack_sent{};
	bool m_ack_pending = false;
	RemoteGamePad::InputAck m_pending_ack{};
	uint32 m_send_seq = 0;
	bool m_media_client = false; // told the media hub about the phone
};
