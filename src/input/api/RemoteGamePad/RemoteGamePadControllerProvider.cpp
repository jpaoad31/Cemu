#include "input/api/RemoteGamePad/RemoteGamePadControllerProvider.h"
#include "input/api/RemoteGamePad/RemoteGamePadController.h"

#include <boost/asio/ip/host_name.hpp>

using namespace std::chrono_literals;
using Peer = RemoteGamePadTransport::Peer;

namespace
{
	// no input for this long: the phone counts as disconnected (buttons released, touch up)
	constexpr auto kConnectionTimeout = 1000ms;
	// latency echo rate (~16 Hz)
	constexpr auto kAckInterval = 60ms;
	// window for the "longest gap" figure
	constexpr auto kGapWindow = 3s;

	uint64 host_now_us()
	{
		return (uint64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}

RemoteGamePadControllerProvider::RemoteGamePadControllerProvider()
	: RemoteGamePadControllerProvider(RemoteGamePadProviderSettings{})
{
}

RemoteGamePadControllerProvider::RemoteGamePadControllerProvider(const RemoteGamePadProviderSettings& settings)
	: base_type(settings)
{
	m_transport = CreateRemoteGamePadUDPTransport(m_settings.port);
	m_status.port = m_settings.port;
	m_status.transport = m_transport->description();
}

RemoteGamePadControllerProvider::~RemoteGamePadControllerProvider()
{
	if (m_running)
	{
		m_running = false;
		if (m_reader_thread.joinable())
			m_reader_thread.join();
	}
	if (m_transport)
		m_transport->close();
}

std::vector<std::shared_ptr<ControllerBase>> RemoteGamePadControllerProvider::get_controllers()
{
	// always offer the controller, so it can be assigned before the phone connects
	start();
	return {std::make_shared<RemoteGamePadController>(m_settings)};
}

void RemoteGamePadControllerProvider::start()
{
	std::scoped_lock lock(m_start_mutex);
	if (m_running)
		return;
	m_running = true;
	m_reader_thread = std::thread(&RemoteGamePadControllerProvider::reader_thread, this);
}

void RemoteGamePadControllerProvider::reader_thread()
{
	SetThreadName("RemoteGamePad");
	std::array<uint8_t, 2048> buffer{};

	while (m_running.load(std::memory_order_relaxed))
	{
		if (!m_transport->is_open())
		{
			std::string error;
			const bool ok = m_transport->open(error);
			{
				std::scoped_lock lock(m_mutex);
				if (!ok && m_status.error != error)
					cemuLog_log(LogType::Force, "Remote GamePad: can't open {}: {}", m_transport->description(), error);
				else if (ok)
					cemuLog_log(LogType::Force, "Remote GamePad: listening on {}", m_transport->description());
				m_status.listening = ok;
				m_status.error = ok ? std::string() : error;
			}
			if (!ok)
			{
				for (int i = 0; i < 20 && m_running; ++i) // retry every 2s
					std::this_thread::sleep_for(100ms);
				continue;
			}
		}

		Peer sender;
		// short timeout, so the latency echo goes out on time even if packets stop
		const size_t size = m_transport->receive(buffer.data(), buffer.size(), sender, 20ms);
		if (size > 0)
			handle_packet(buffer.data(), size, sender);

		send_input_ack();

		// timeouts, packet rate, gaps
		std::scoped_lock lock(m_mutex);
		const auto now = std::chrono::steady_clock::now();
		if (m_state.connected && now - m_last_input > kConnectionTimeout)
		{
			m_state.connected = false;
			m_state.input = {};
			m_latch.Clear();
			m_have_seq = false;
			cemuLog_log(LogType::Force, "Remote GamePad: \"{}\" timed out", m_status.phone_name);
		}
		m_status.connected = m_state.connected;
		const auto window = now - m_rate_window_start;
		if (window >= 1s)
		{
			m_status.packet_rate = (float)m_rate_count / std::chrono::duration<float>(window).count();
			m_rate_count = 0;
			m_rate_window_start = now;
		}
		while (!m_gaps.empty() && now - m_gaps.front().first > kGapWindow)
			m_gaps.pop_front();
		float max_gap = 0;
		for (const auto& g : m_gaps)
			max_gap = std::max(max_gap, g.second);
		// a stall that is still going on counts too
		if (m_state.connected)
			max_gap = std::max(max_gap, std::chrono::duration<float, std::milli>(now - m_last_input).count());
		m_status.max_gap_ms = max_gap;
	}
}

void RemoteGamePadControllerProvider::handle_packet(const uint8_t* data, size_t size, const Peer& sender)
{
	using namespace RemoteGamePad;
	const auto header = ParseHeader(data, size);
	if (!header)
		return;

	switch (header->type)
	{
	case MsgType::Hello:
	{
		const auto hello = ParseHello(data, size);
		if (!hello)
			return;
		{
			std::scoped_lock lock(m_mutex);
			if (hello->client_id != m_client_id || sender != m_client)
			{
				cemuLog_log(LogType::Force, "Remote GamePad: hello from \"{}\" ({}, app {}) at {}", hello->device_name,
					hello->device_model, hello->app_version, sender.address);
				m_client_id = hello->client_id;
				m_client = sender;
				m_have_seq = false;
				m_last_motion_ts = 0;
				m_motion_handler = {};
				m_gaps.clear();
			}
			m_status.phone_name = hello->device_name;
			m_status.phone_model = hello->device_model;
			m_status.app_version = hello->app_version;
			m_status.address = sender.address;
		}
		send_hello_ack(hello->client_id, sender);
		break;
	}
	case MsgType::Input:
	{
		const auto input = ParseInput(data, size);
		if (!input)
			return;
		std::scoped_lock lock(m_mutex);
		const auto now = std::chrono::steady_clock::now();
		if (sender != m_client)
		{
			// input before (or without) a hello, or another phone took over
			m_client = sender;
			m_status.address = sender.address;
			m_have_seq = false;
			m_last_motion_ts = 0;
			m_gaps.clear();
		}
		// drop late (reordered) packets
		if (m_have_seq && !SeqNewer(header->seq, m_last_seq) && now - m_last_input < kConnectionTimeout)
			return;
		if (m_have_seq && m_state.connected)
			m_gaps.emplace_back(now, std::chrono::duration<float, std::milli>(now - m_last_input).count());
		m_have_seq = true;
		m_last_seq = header->seq;
		m_last_input = now;
		m_rate_count++;
		m_status.packets++;
		if (!m_state.connected)
			cemuLog_log(LogType::Force, "Remote GamePad: receiving input from {}", m_status.address);
		m_latch.OnInput(m_state.input, m_state.connected, *input, host_now_us());
		m_state.connected = true;
		m_state.input = *input;

		// latency echo: the newest packet is acked at the next ack slot
		m_pending_ack.acked_seq = header->seq;
		m_pending_ack.phone_timestamp_us = input->timestamp_us;
		m_pending_ack.host_receive_us = host_now_us();
		m_ack_pending = true;

		if (input->motion_valid())
		{
			if (m_last_motion_ts != 0 && input->timestamp_us > m_last_motion_ts)
			{
				float dt = (float)(input->timestamp_us - m_last_motion_ts) / 1000000.0f;
				dt = std::min(dt, 0.1f); // after a gap, don't integrate a huge step
				const auto m = ToMotionHandlerFrame(input->gyro, input->accel);
				m_motion_handler.processMotionSample(dt, m.gx, m.gy, m.gz, m.ax, m.ay, m.az);
				m_state.motion = m_motion_handler.getMotionSample();
			}
			if (input->timestamp_us > m_last_motion_ts || input->timestamp_us + 10000000 < m_last_motion_ts)
				m_last_motion_ts = input->timestamp_us; // forward, or the sender's clock restarted
		}
		break;
	}
	case MsgType::Bye:
	{
		std::scoped_lock lock(m_mutex);
		if (sender == m_client)
		{
			cemuLog_log(LogType::Force, "Remote GamePad: \"{}\" disconnected", m_status.phone_name);
			m_state.connected = false;
			m_state.input = {};
			m_latch.Clear();
			m_have_seq = false;
			m_ack_pending = false;
			m_status.connected = false;
		}
		break;
	}
	default:
		break; // unknown/future message types are ignored
	}
}

void RemoteGamePadControllerProvider::send_input_ack()
{
	RemoteGamePad::InputAck ack;
	Peer to;
	{
		std::scoped_lock lock(m_mutex);
		const auto now = std::chrono::steady_clock::now();
		if (!m_ack_pending || now - m_last_ack_sent < kAckInterval)
			return;
		m_ack_pending = false;
		m_last_ack_sent = now;
		ack = m_pending_ack;
		ack.host_packet_rate = (uint16)std::clamp(m_status.packet_rate + 0.5f, 0.0f, 65535.0f);
		ack.host_max_gap_ms = (uint16)std::clamp(m_status.max_gap_ms + 0.5f, 0.0f, 65535.0f);
		to = m_client;
	}
	const auto packet = RemoteGamePad::BuildInputAck(m_send_seq++, ack);
	m_transport->send(to, packet.data(), packet.size());
}

void RemoteGamePadControllerProvider::send_hello_ack(uint32 client_id, const Peer& to)
{
	RemoteGamePad::HelloAck ack;
	ack.client_id = client_id;
	ack.capabilities = RemoteGamePad::kCapMotion | RemoteGamePad::kCapTouch;
	ack.video_port = 0;
	boost::system::error_code ec;
	const auto host = boost::asio::ip::host_name(ec);
	ack.host_name = ec ? std::string("Cemu") : fmt::format("Cemu on {}", host);
	const auto packet = RemoteGamePad::BuildHelloAck(m_send_seq++, ack);
	if (!m_transport->send(to, packet.data(), packet.size()))
		cemuLog_log(LogType::Force, "Remote GamePad: can't answer the hello from {}", to.address);
}

bool RemoteGamePadControllerProvider::is_connected() const
{
	std::scoped_lock lock(m_mutex);
	return m_state.connected && std::chrono::steady_clock::now() - m_last_input <= kConnectionTimeout;
}

RemoteGamePadControllerProvider::State RemoteGamePadControllerProvider::get_state() const
{
	std::scoped_lock lock(m_mutex);
	const auto now = std::chrono::steady_clock::now();
	if (!m_state.connected || now - m_last_input > kConnectionTimeout)
		return {};
	State s = m_state;
	const auto latch = m_latch.Get(host_now_us());
	s.latched_buttons = latch.buttons;
	s.latched_touch = latch.touch;
	s.latched_touch_x = latch.touch_x;
	s.latched_touch_y = latch.touch_y;
	s.touch_latch_seq = latch.touch_seq;
	return s;
}

void RemoteGamePadControllerProvider::consume_latch(uint32 buttons, bool touch, uint32 touch_latch_seq)
{
	std::scoped_lock lock(m_mutex);
	m_latch.Consume(buttons, touch, touch_latch_seq);
}

RemoteGamePadControllerProvider::Status RemoteGamePadControllerProvider::get_status() const
{
	std::scoped_lock lock(m_mutex);
	auto status = m_status;
	status.connected = m_state.connected && std::chrono::steady_clock::now() - m_last_input <= kConnectionTimeout;
	if (!status.connected)
		status.packet_rate = 0;
	else
	{
		status.buttons = m_state.input.buttons;
		status.touch = m_state.input.touch_down();
		status.touch_x = m_state.input.touch_x;
		status.touch_y = m_state.input.touch_y;
	}
	return status;
}

std::string RemoteGamePadControllerProvider::get_status_text() const
{
	const auto s = get_status();
	if (!m_running)
		return fmt::format("Remote GamePad: not started ({})", s.transport);
	if (!s.listening)
		return fmt::format("Can't listen on {}: {}", s.transport, s.error.empty() ? "starting" : s.error);
	if (!s.connected)
	{
		if (!s.phone_name.empty())
			return fmt::format("Listening on {}.\nLast phone: {} ({}), not sending.", s.transport, s.phone_name, s.address);
		return fmt::format("Listening on {}.\nWaiting for the phone...", s.transport);
	}
	const auto name = s.phone_name.empty() ? std::string("unnamed phone") : s.phone_name;
	const auto touch = s.touch ? fmt::format(", touch {:.3f},{:.3f}", s.touch_x, s.touch_y) : std::string();
	return fmt::format("Listening on {}. Connected: {} ({})\n{:.0f} packets/s, longest gap {:.0f} ms (last 3 s)\nHeld: {}{}",
		s.transport, name, s.address, s.packet_rate, s.max_gap_ms, RemoteGamePad::ButtonNames(s.buttons), touch);
}
