#include "input/api/RemoteGamePad/RemoteGamePadTransport.h"

#include <boost/asio.hpp>
#include <array>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

#if defined(__APPLE__)
#include <dns_sd.h>
#include <arpa/inet.h>
#endif

using boost::asio::ip::udp;

#if !defined(_WIN32)
#include "input/api/RemoteGamePad/RemoteGamePadUdp.h"
#endif

namespace
{
#if !defined(_WIN32)
	// POSIX: replies leave from the address the request was sent to (RemoteGamePadUdp.h)
	class UDPTransport : public RemoteGamePadTransport
	{
	public:
		explicit UDPTransport(uint16_t port) : m_port(port) {}
		~UDPTransport() override { close(); }

		bool open(std::string& error) override
		{
			close();
			// interactive-video service class (Wi-Fi video access category) for the controls too
			if (!m_udp.Open(m_port, error, true))
				return false;
			advertise();
			return true;
		}

		bool is_open() const override { return m_udp.IsOpen(); }

		void close() override
		{
			m_udp.Close();
#if defined(__APPLE__)
			if (m_dnssd)
			{
				DNSServiceRefDeallocate(m_dnssd);
				m_dnssd = nullptr;
			}
#endif
		}

		size_t receive(uint8_t* buffer, size_t capacity, Peer& from, std::chrono::milliseconds timeout) override
		{
			RemoteGamePad::Net::UdpAddr addr;
			const size_t size = m_udp.Receive(buffer, capacity, addr, (int)timeout.count());
			if (size == 0)
				return 0;
			from.address = addr.Address();
			from.id = addr.Id();
			std::scoped_lock lock(m_peers_mutex);
			if (m_peers.size() > 64 && !m_peers.contains(from.id))
				m_peers.clear();
			m_peers[from.id] = addr; // the newest local address the peer used
			return size;
		}

		bool send(const Peer& to, const uint8_t* data, size_t size) override
		{
			RemoteGamePad::Net::UdpAddr addr;
			{
				std::scoped_lock lock(m_peers_mutex);
				const auto it = m_peers.find(to.id);
				if (it == m_peers.end())
					return false;
				addr = it->second;
			}
			return m_udp.Send(addr, data, size);
		}

		std::string description() const override
		{
			return "UDP port " + std::to_string(m_port) + (m_udp.HasV6() ? " (IPv4+IPv6)" : "");
		}

	private:
		void advertise()
		{
#if defined(__APPLE__)
			if (m_dnssd)
				return;
			const DNSServiceErrorType err = DNSServiceRegister(&m_dnssd, 0, 0, nullptr, "_wiiugamepad._udp", nullptr, nullptr,
				htons(m_port), 0, nullptr, nullptr, nullptr);
			if (err != kDNSServiceErr_NoError)
				m_dnssd = nullptr;
#endif
		}

#if defined(__APPLE__)
		DNSServiceRef m_dnssd = nullptr;
#endif
		uint16_t m_port;
		RemoteGamePad::Net::UdpDual m_udp;
		std::mutex m_peers_mutex;
		std::unordered_map<std::string, RemoteGamePad::Net::UdpAddr> m_peers;
	};
#else
	// Two sockets on the same port: IPv4 (exactly as in phase 1) and IPv6-only. Not one dual-stack
	// socket: replies to IPv4-mapped addresses from an IPv6 socket weren't reaching the phone. Each
	// reply goes out of the socket (and so the family and port) the packet came in on.
	class UDPTransport : public RemoteGamePadTransport
	{
	public:
		explicit UDPTransport(uint16_t port) : m_port(port), m_v4{udp::socket(m_io)}, m_v6{udp::socket(m_io)} {}
		~UDPTransport() override { close(); }

		bool open(std::string& error) override
		{
			boost::system::error_code ec;
			close();
			m_v4.socket.open(udp::v4(), ec);
			if (!ec)
				m_v4.socket.bind(udp::endpoint(udp::v4(), m_port), ec);
			if (ec)
			{
				boost::system::error_code ignored;
				m_v4.socket.close(ignored);
				error = ec.message();
				return false;
			}
			// IPv6 as well (Bonjour on the phone may resolve the Mac to an IPv6, often link-local, address)
			boost::system::error_code ec6;
			m_v6.socket.open(udp::v6(), ec6);
			if (!ec6)
				m_v6.socket.set_option(boost::asio::ip::v6_only(true), ec6);
			if (!ec6)
				m_v6.socket.bind(udp::endpoint(udp::v6(), m_port), ec6);
			if (ec6)
			{
				boost::system::error_code ignored;
				m_v6.socket.close(ignored);
			}
			m_io.restart();
			arm(m_v4);
			if (m_v6.socket.is_open())
				arm(m_v6);
			advertise();
			return true;
		}

		bool is_open() const override { return m_v4.socket.is_open(); }

		void close() override
		{
			boost::system::error_code ec;
			if (m_v4.socket.is_open())
				m_v4.socket.close(ec);
			if (m_v6.socket.is_open())
				m_v6.socket.close(ec);
			// let the aborted receive handlers run before the buffers can be reused
			m_io.restart();
			m_io.poll();
			m_received.clear();
#if defined(__APPLE__)
			if (m_dnssd)
			{
				DNSServiceRefDeallocate(m_dnssd);
				m_dnssd = nullptr;
			}
#endif
		}

		size_t receive(uint8_t* buffer, size_t capacity, Peer& from, std::chrono::milliseconds timeout) override
		{
			if (m_received.empty())
			{
				if (m_io.stopped())
					m_io.restart();
				m_io.run_one_for(timeout);
				m_io.poll(); // anything else already there
			}
			if (m_received.empty())
				return 0;
			auto [data, sender] = std::move(m_received.front());
			m_received.pop_front();
			const size_t size = std::min(capacity, data.size());
			std::memcpy(buffer, data.data(), size);
			from.address = sender.address().to_string();
			from.id = from.address + ":" + std::to_string(sender.port());
			{
				std::scoped_lock lock(m_peers_mutex);
				if (m_peers.size() > 64 && !m_peers.contains(from.id))
					m_peers.clear();
				m_peers[from.id] = sender;
			}
			return size;
		}

		bool send(const Peer& to, const uint8_t* data, size_t size) override
		{
			udp::endpoint ep;
			{
				std::scoped_lock lock(m_peers_mutex);
				const auto it = m_peers.find(to.id);
				if (it == m_peers.end())
					return false;
				ep = it->second;
			}
			auto& sock = ep.address().is_v4() ? m_v4.socket : m_v6.socket;
			if (!sock.is_open())
				return false;
			boost::system::error_code ec;
			sock.send_to(boost::asio::buffer(data, size), ep, 0, ec);
			return !ec;
		}

		std::string description() const override
		{
			return "UDP port " + std::to_string(m_port) + (m_v6.socket.is_open() ? " (IPv4+IPv6)" : "");
		}

	private:
		struct Socket
		{
			udp::socket socket;
			std::array<uint8_t, 2048> buffer{};
			udp::endpoint from;
		};

		// keep one receive pending per socket; packets queue in m_received (reader thread only)
		void arm(Socket& k)
		{
			k.socket.async_receive_from(boost::asio::buffer(k.buffer), k.from, [this, &k](const boost::system::error_code& ec, size_t n) {
				if (!ec && n > 0 && m_received.size() < 256)
					m_received.emplace_back(std::vector<uint8_t>(k.buffer.begin(), k.buffer.begin() + n), k.from);
				if (ec != boost::asio::error::operation_aborted && k.socket.is_open())
					arm(k);
			});
		}

		// Bonjour, so the phone can find the Mac without typing an IP (macOS only, best effort)
		void advertise()
		{
#if defined(__APPLE__)
			if (m_dnssd)
				return;
			// empty name: the system uses the computer name
			const DNSServiceErrorType err = DNSServiceRegister(&m_dnssd, 0, 0, nullptr, "_wiiugamepad._udp", nullptr, nullptr,
				htons(m_port), 0, nullptr, nullptr, nullptr);
			if (err != kDNSServiceErr_NoError)
				m_dnssd = nullptr;
#endif
		}

#if defined(__APPLE__)
		DNSServiceRef m_dnssd = nullptr;
#endif
		uint16_t m_port;
		boost::asio::io_context m_io;
		Socket m_v4, m_v6;
		std::deque<std::pair<std::vector<uint8_t>, udp::endpoint>> m_received;
		std::mutex m_peers_mutex;
		std::unordered_map<std::string, udp::endpoint> m_peers;
	};
#endif
}

std::unique_ptr<RemoteGamePadTransport> CreateRemoteGamePadUDPTransport(uint16_t port)
{
	return std::make_unique<UDPTransport>(port);
}
