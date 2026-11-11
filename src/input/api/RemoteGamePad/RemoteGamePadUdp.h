#pragma once

// UDP for the Remote GamePad on POSIX systems: an IPv4 socket and an IPv6-only socket on one
// port, and every reply leaves from the local address (and interface) the request came in on.
//
// Why: the phone's NWConnection is a connected UDP socket and drops any reply whose source isn't
// exactly the address it sent to. A socket bound to "any" lets the routing table pick the reply's
// source, so on a Mac with two interfaces on the same subnet (Wi-Fi 192.168.0.88 and Ethernet
// 192.168.0.174) a request to .88 got its reply from .174 and the phone never saw it. Here the
// destination address of each request is read with IP_PKTINFO / IPV6_PKTINFO and set as the
// source of the reply.
//
// No Cemu dependencies (compiled without the precompiled header).

#include <sys/socket.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace RemoteGamePad::Net
{
	struct UdpAddr
	{
		sockaddr_storage remote{};
		socklen_t remote_len = 0;
		// where the packet was sent to (our address) and on which interface
		sockaddr_storage local{};
		bool has_local = false;
		uint32_t ifindex = 0;

		bool SameRemote(const UdpAddr& o) const;
		std::string Address() const; // "192.168.0.20" (IPv4-mapped shown as IPv4), "fe80::1%en0"
		std::string Id() const;      // address + ":" + port
		std::string LocalAddress() const;
	};

	class UdpDual
	{
	public:
		~UdpDual() { Close(); }
		bool Open(uint16_t port, std::string& error, bool video_service_class = false);
		void Close();
		bool IsOpen() const { return m_fd4 >= 0; }
		bool HasV6() const { return m_fd6 >= 0; }
		// waits up to timeout_ms for a packet; returns its size, 0 on timeout
		size_t Receive(uint8_t* buffer, size_t capacity, UdpAddr& from, int timeout_ms);
		// from the socket of the destination's family, with the request's local address as source
		bool Send(const UdpAddr& to, const uint8_t* data, size_t size) const;

	private:
		size_t ReceiveFrom(int fd, uint8_t* buffer, size_t capacity, UdpAddr& from);
		int m_fd4 = -1, m_fd6 = -1;
		bool m_prefer6 = false; // alternate between the sockets when both are readable
	};
}
