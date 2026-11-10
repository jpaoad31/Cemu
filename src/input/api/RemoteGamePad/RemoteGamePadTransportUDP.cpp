#include "input/api/RemoteGamePad/RemoteGamePadTransport.h"

#include <boost/asio.hpp>
#include <mutex>
#include <unordered_map>

#if defined(__APPLE__)
#include <dns_sd.h>
#include <arpa/inet.h>
#endif

using boost::asio::ip::udp;

namespace
{
	class UDPTransport : public RemoteGamePadTransport
	{
	public:
		explicit UDPTransport(uint16_t port) : m_port(port), m_socket(m_io) {}
		~UDPTransport() override { close(); }

		bool open(std::string& error) override
		{
			boost::system::error_code ec;
			close();
			m_socket.open(udp::v4(), ec);
			if (!ec)
				m_socket.bind(udp::endpoint(udp::v4(), m_port), ec);
			if (ec)
			{
				boost::system::error_code ignored;
				m_socket.close(ignored);
				error = ec.message();
				return false;
			}
			advertise();
			return true;
		}

		bool is_open() const override { return m_socket.is_open(); }

		void close() override
		{
			boost::system::error_code ec;
			if (m_socket.is_open())
				m_socket.close(ec);
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
			bool done = false;
			size_t size = 0;
			boost::system::error_code rec_ec;
			udp::endpoint sender;
			m_io.restart();
			m_socket.async_receive_from(boost::asio::buffer(buffer, capacity), sender,
				[&](const boost::system::error_code& ec, size_t n) {
					rec_ec = ec;
					size = n;
					done = true;
				});
			m_io.run_for(timeout);
			if (!done)
			{
				// timed out: cancel, and let the aborted handler run before the buffer is reused
				// (a packet that raced the cancel still arrives with no error)
				boost::system::error_code ec;
				m_socket.cancel(ec);
				m_io.restart();
				m_io.run();
			}
			if (!done || rec_ec || size == 0)
				return 0;
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
			boost::system::error_code ec;
			m_socket.send_to(boost::asio::buffer(data, size), ep, 0, ec);
			return !ec;
		}

		std::string description() const override { return "UDP port " + std::to_string(m_port); }

	private:
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
		udp::socket m_socket;
		std::mutex m_peers_mutex;
		std::unordered_map<std::string, udp::endpoint> m_peers;
	};
}

std::unique_ptr<RemoteGamePadTransport> CreateRemoteGamePadUDPTransport(uint16_t port)
{
	return std::make_unique<UDPTransport>(port);
}
