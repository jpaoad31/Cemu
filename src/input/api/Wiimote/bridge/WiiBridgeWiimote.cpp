#include "input/api/Wiimote/bridge/WiiBridgeWiimote.h"

#include "config/CemuConfig.h"
#include "util/helpers/helpers.h"

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

// Message types, from the bridge's docs/protocol.md (protocol version 1)
namespace
{
	constexpr uint8 kProtocolVersion = 1;

	// host -> bridge
	constexpr uint8 MSG_HELLO = 0x01;       // [version] keepalive, answered by HELLO_ACK
	constexpr uint8 MSG_SCAN_START = 0x02;  // [seconds]
	constexpr uint8 MSG_SCAN_STOP = 0x03;
	constexpr uint8 MSG_DATA_OUT = 0x10;    // [slot][report...] without the 0xA2 header

	// bridge -> host
	constexpr uint8 MSG_HELLO_ACK = 0x81;   // [version][fw][bt addr 6][slot count][scanning][state per slot...]
	constexpr uint8 MSG_SCAN_STATE = 0x82;  // [scanning]
	constexpr uint8 MSG_CONNECTED = 0x83;   // [slot][bt addr 6][name...]
	constexpr uint8 MSG_DISCONNECTED = 0x84;// [slot][reason]
	constexpr uint8 MSG_DATA_IN = 0x90;     // [slot][report...] without the 0xA1 header
	constexpr uint8 MSG_LOG = 0xF0;         // [text...]

	constexpr uint8 SLOT_CONNECTED = 2;
	constexpr size_t kMaxSlots = 16;        // the bridge reports its slot count (7 on the Pico)
	constexpr size_t kMaxQueuedReports = 512;
	constexpr auto kHelloInterval = std::chrono::milliseconds(500);
	constexpr auto kBridgeTimeout = std::chrono::seconds(3);

	std::string format_address(const uint8* a)
	{
		return fmt::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", a[0], a[1], a[2], a[3], a[4], a[5]);
	}
}

// shared by the session thread and the WiiBridgeWiimote objects handed to the Wiimote provider
struct WiiBridgeDeviceState
{
	uint8 slot = 0;
	std::string address;   // "34:AF:2C:E6:C1:87"
	std::string name;      // "Nintendo RVL-WBC-01"
	std::string endpoint;  // the bridge, for get_location()

	std::atomic_bool alive = true;
	std::mutex mutex;
	std::deque<std::vector<uint8>> reports;
	uint64 dropped_reports = 0;
};

namespace
{
	class Session
	{
	public:
		Session(std::string host, uint16 port)
			: m_host(std::move(host)), m_port(port), m_endpoint_text(fmt::format("{}:{}", m_host, port))
		{
			m_thread = std::thread(&Session::run, this);
		}

		~Session()
		{
			m_running = false;
			if (m_thread.joinable())
				m_thread.join();
			std::scoped_lock lock(m_mutex);
			for (auto& slot : m_slots)
				if (slot)
					slot->alive = false;
		}

		bool same_target(const std::string& host, uint16 port) const { return host == m_host && port == m_port; }
		const std::string& endpoint() const { return m_endpoint_text; }

		std::vector<std::shared_ptr<WiiBridgeDeviceState>> devices()
		{
			std::scoped_lock lock(m_mutex);
			std::vector<std::shared_ptr<WiiBridgeDeviceState>> result;
			for (auto& slot : m_slots)
				if (slot && slot->alive)
					result.push_back(slot);
			return result;
		}

		bool send(const std::vector<uint8>& message)
		{
			std::scoped_lock lock(m_send_mutex);
			if (!m_socket || !m_target_valid)
				return false;
			boost::system::error_code ec;
			m_socket->send_to(boost::asio::buffer(message), m_target, 0, ec);
			if (ec && !m_send_error_logged)
			{
				m_send_error_logged = true;
				cemuLog_log(LogType::InputAPI, "WiiBridge: can't send to the bridge at {}: {}", m_endpoint_text, ec.message());
			}
			else if (!ec)
				m_send_error_logged = false;
			return !ec;
		}

		void fill_status(WiiBridge::Status& status)
		{
			std::scoped_lock lock(m_mutex);
			status.endpoint = m_endpoint_text;
			status.bridge_answering = m_last_ack != std::chrono::steady_clock::time_point{} &&
				std::chrono::steady_clock::now() - m_last_ack < kBridgeTimeout;
			status.firmware_version = m_firmware_version;
			status.bridge_address = m_bridge_address;
			status.scanning = m_scanning;
			status.last_log = m_last_log;
			for (auto& slot : m_slots)
				if (slot && slot->alive)
					status.devices.push_back(fmt::format("slot {}: {} ({})", slot->slot, slot->name, slot->address));
		}

	private:
		void run()
		{
			SetThreadName("WiiBridge");
			using namespace boost::asio;
			io_context io;
			{
				std::scoped_lock lock(m_send_mutex);
				try
				{
					ip::udp::resolver resolver(io);
					m_target = *resolver.resolve(ip::udp::v4(), m_host, fmt::format("{}", m_port)).begin();
					m_socket = std::make_unique<ip::udp::socket>(io);
					m_socket->open(ip::udp::v4());
					m_socket->bind(ip::udp::endpoint(ip::udp::v4(), 0));
					m_socket->non_blocking(true);
					m_target_valid = true;
					cemuLog_log(LogType::InputAPI, "WiiBridge: session started, bridge at {} (local port {})", m_endpoint_text, m_socket->local_endpoint().port());
				}
				catch (const std::exception& ex)
				{
					cemuLog_log(LogType::InputAPI, "WiiBridge: can't set up the bridge connection to {}: {}", m_endpoint_text, ex.what());
				}
			}

			std::array<uint8, 2048> buffer;
			auto last_hello = std::chrono::steady_clock::time_point{};
			bool was_answering = false;
			while (m_running)
			{
				const auto now = std::chrono::steady_clock::now();
				if (now - last_hello >= kHelloInterval)
				{
					last_hello = now;
					send({MSG_HELLO, kProtocolVersion});

					// bridge gone (unplugged, rebooted, wrong address): its devices are gone too
					std::scoped_lock lock(m_mutex);
					const bool answering = m_last_ack != std::chrono::steady_clock::time_point{} && now - m_last_ack < kBridgeTimeout;
					if (was_answering && !answering)
					{
						cemuLog_log(LogType::InputAPI, "WiiBridge: the bridge at {} stopped answering", m_endpoint_text);
						for (auto& slot : m_slots)
							if (slot)
								slot->alive = false;
						m_slots = {};
					}
					was_answering = answering;
				}

				if (!m_socket || !m_target_valid)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					continue;
				}

				bool received = false;
				for (int i = 0; i < 64; ++i)
				{
					boost::system::error_code ec;
					ip::udp::endpoint sender;
					const size_t len = m_socket->receive_from(boost::asio::buffer(buffer), sender, 0, ec);
					if (ec)
						break;
					received = true;
					if (len > 0)
						handle_message(buffer.data(), len);
				}
				if (!received)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		void handle_message(const uint8* m, size_t len)
		{
			switch (m[0])
			{
			case MSG_HELLO_ACK:
			{
				if (len < 11)
					return;
				std::scoped_lock lock(m_mutex);
				const auto now = std::chrono::steady_clock::now();
				// first answer, or answering again after it went quiet (unplugged, rebooted)
				const bool first = m_last_ack == std::chrono::steady_clock::time_point{} || now - m_last_ack >= kBridgeTimeout;
				m_last_ack = now;
				m_firmware_version = m[2];
				m_bridge_address = format_address(m + 3);
				m_scanning = m[10] != 0;
				const size_t slot_count = std::min<size_t>(m[9], len - 11);
				if (first || m_logged_version != m[1])
				{
					m_logged_version = m[1];
					cemuLog_log(LogType::InputAPI, "WiiBridge: bridge {} answering: protocol {}, firmware {}, {} slots{}", m_bridge_address, m[1], m[2], m[9],
						m[1] != kProtocolVersion ? fmt::format(" (Cemu speaks protocol {})", kProtocolVersion) : "");
				}
				// a slot the bridge says is not connected (a DISCONNECTED we missed): drop it
				for (size_t i = 0; i < std::min(slot_count, kMaxSlots); ++i)
				{
					if (m[11 + i] != SLOT_CONNECTED && m_slots[i])
					{
						cemuLog_log(LogType::InputAPI, "WiiBridge: slot {} ({}) is no longer connected", i, m_slots[i]->address);
						m_slots[i]->alive = false;
						m_slots[i].reset();
					}
				}
				break;
			}
			case MSG_SCAN_STATE:
			{
				if (len < 2)
					return;
				std::scoped_lock lock(m_mutex);
				m_scanning = m[1] != 0;
				cemuLog_log(LogType::InputAPI, "WiiBridge: scan {}", m_scanning ? "started" : "stopped");
				break;
			}
			case MSG_CONNECTED:
			{
				if (len < 8 || m[1] >= kMaxSlots)
					return;
				const uint8 slot = m[1];
				const std::string address = format_address(m + 2);
				const std::string name(reinterpret_cast<const char*>(m + 8), len - 8);
				std::scoped_lock lock(m_mutex);
				auto& current = m_slots[slot];
				// the bridge replays CONNECTED for every device when it sees us (again): nothing new
				if (current && current->alive && current->address == address)
					return;
				if (current)
					current->alive = false;
				// the same device can't be in two slots: drop an old entry for it
				for (auto& other : m_slots)
				{
					if (other && other->address == address)
					{
						other->alive = false;
						other.reset();
					}
				}
				auto state = std::make_shared<WiiBridgeDeviceState>();
				state->slot = slot;
				state->address = address;
				state->name = name;
				state->endpoint = m_endpoint_text;
				current = state;
				cemuLog_log(LogType::InputAPI, "WiiBridge: slot {} connected: \"{}\" ({})", slot, name, address);
				break;
			}
			case MSG_DISCONNECTED:
			{
				if (len < 3 || m[1] >= kMaxSlots)
					return;
				std::scoped_lock lock(m_mutex);
				auto& current = m_slots[m[1]];
				if (current)
				{
					cemuLog_log(LogType::InputAPI, "WiiBridge: slot {} disconnected: \"{}\" ({}), reason 0x{:02x}", m[1], current->name, current->address, m[2]);
					current->alive = false;
					current.reset();
				}
				break;
			}
			case MSG_DATA_IN:
			{
				if (len < 3 || m[1] >= kMaxSlots)
					return;
				std::shared_ptr<WiiBridgeDeviceState> state;
				{
					std::scoped_lock lock(m_mutex);
					state = m_slots[m[1]];
				}
				if (!state)
					return;
				std::scoped_lock lock(state->mutex);
				if (state->reports.size() >= kMaxQueuedReports)
				{
					state->reports.pop_front();
					if (state->dropped_reports++ % 1000 == 0)
						cemuLog_log(LogType::InputAPI, "WiiBridge: slot {}: reports not read fast enough, dropping old ones ({} so far)", m[1], state->dropped_reports);
				}
				state->reports.emplace_back(m + 2, m + len);
				break;
			}
			case MSG_LOG:
			{
				std::string text(reinterpret_cast<const char*>(m + 1), len - 1);
				cemuLog_log(LogType::InputAPI, "WiiBridge: bridge log: {}", text);
				std::scoped_lock lock(m_mutex);
				m_last_log = std::move(text);
				break;
			}
			default:
				break;
			}
		}

		const std::string m_host;
		const uint16 m_port;
		const std::string m_endpoint_text;
		std::atomic_bool m_running = true;
		std::thread m_thread;

		std::mutex m_send_mutex;
		std::unique_ptr<boost::asio::ip::udp::socket> m_socket;
		boost::asio::ip::udp::endpoint m_target;
		bool m_target_valid = false;
		bool m_send_error_logged = false;

		std::mutex m_mutex;
		std::array<std::shared_ptr<WiiBridgeDeviceState>, kMaxSlots> m_slots{};
		std::chrono::steady_clock::time_point m_last_ack{};
		int m_logged_version = -1;
		int m_firmware_version = 0;
		std::string m_bridge_address;
		bool m_scanning = false;
		std::string m_last_log;
	};

	std::mutex s_session_mutex;
	std::unique_ptr<Session> s_session;
	bool s_from_environment = false;

	// CEMU_WII_BRIDGE=host[:port] (e.g. 127.0.0.1:4731), for testing without the settings UI
	std::optional<std::pair<std::string, uint16>> environment_target()
	{
		const char* env = std::getenv("CEMU_WII_BRIDGE");
		if (!env || !*env)
			return {};
		std::string value(env);
		uint16 port = 4730;
		if (const auto colon = value.rfind(':'); colon != std::string::npos)
		{
			port = (uint16)std::strtoul(value.c_str() + colon + 1, nullptr, 10);
			value.resize(colon);
		}
		return std::make_pair(value, port);
	}

	Session* session_locked()
	{
		return s_session.get();
	}
}

// ---------------------------------------------------------------- WiiBridge

void WiiBridge::apply_settings()
{
	bool enabled;
	std::string host;
	uint16 port;
	if (const auto env = environment_target())
	{
		enabled = true;
		host = env->first;
		port = env->second;
	}
	else
	{
		const auto& config = GetConfig();
		enabled = config.wii_bridge.enabled;
		host = config.wii_bridge.host.GetValue();
		port = config.wii_bridge.port;
	}

	std::scoped_lock lock(s_session_mutex);
	s_from_environment = environment_target().has_value();
	if (!enabled || host.empty() || port == 0)
	{
		if (s_session)
		{
			cemuLog_log(LogType::InputAPI, "WiiBridge: disabled, closing the session with {}", s_session->endpoint());
			s_session.reset();
		}
		return;
	}
	if (s_session && s_session->same_target(host, port))
		return;
	s_session.reset();
	s_session = std::make_unique<Session>(host, port);
}

void WiiBridge::start_scan(int seconds)
{
	apply_settings();
	std::scoped_lock lock(s_session_mutex);
	if (auto* session = session_locked())
	{
		cemuLog_log(LogType::InputAPI, "WiiBridge: asking the bridge to scan for {} s", seconds);
		session->send({MSG_SCAN_START, (uint8)std::clamp(seconds, 1, 120)});
	}
}

void WiiBridge::stop_scan()
{
	std::scoped_lock lock(s_session_mutex);
	if (auto* session = session_locked())
		session->send({MSG_SCAN_STOP});
}

WiiBridge::Status WiiBridge::get_status()
{
	Status status;
	std::scoped_lock lock(s_session_mutex);
	status.from_environment = s_from_environment;
	if (auto* session = session_locked())
	{
		status.enabled = true;
		session->fill_status(status);
	}
	return status;
}

// ---------------------------------------------------------------- WiiBridgeWiimote

WiiBridgeWiimote::WiiBridgeWiimote(std::shared_ptr<WiiBridgeDeviceState> state)
	: m_state(std::move(state))
{
}

bool WiiBridgeWiimote::write_data(const std::vector<uint8>& data)
{
	if (!m_state->alive || data.empty())
		return false;
	std::vector<uint8> message;
	message.reserve(data.size() + 2);
	message.push_back(MSG_DATA_OUT);
	message.push_back(m_state->slot);
	message.insert(message.end(), data.begin(), data.end());
	std::scoped_lock lock(s_session_mutex);
	auto* session = session_locked();
	return session && session->send(message);
}

std::optional<std::vector<uint8>> WiiBridgeWiimote::read_data()
{
	std::scoped_lock lock(m_state->mutex);
	if (!m_state->reports.empty())
	{
		auto report = std::move(m_state->reports.front());
		m_state->reports.pop_front();
		return report;
	}
	if (!m_state->alive)
		return std::nullopt;
	return std::vector<uint8>{};
}

bool WiiBridgeWiimote::operator==(const WiimoteDevice& o) const
{
	const auto* other = dynamic_cast<const WiiBridgeWiimote*>(&o);
	return other && other->m_state->address == m_state->address;
}

std::string WiiBridgeWiimote::get_product_name() const
{
	return m_state->name;
}

std::string WiiBridgeWiimote::get_location() const
{
	return fmt::format("bridge {} slot {} ({})", m_state->endpoint, m_state->slot, m_state->address);
}

std::string WiiBridgeWiimote::get_address() const
{
	return m_state->address;
}

std::vector<WiimoteDevicePtr> WiiBridgeWiimote::get_devices()
{
	WiiBridge::apply_settings();
	std::vector<std::shared_ptr<WiiBridgeDeviceState>> states;
	{
		std::scoped_lock lock(s_session_mutex);
		if (auto* session = session_locked())
			states = session->devices();
	}
	std::vector<WiimoteDevicePtr> devices;
	devices.reserve(states.size());
	for (auto& state : states)
		devices.push_back(std::make_shared<WiiBridgeWiimote>(state));
	return devices;
}
