// See RemoteGamePadUdp.h. Compiled without the precompiled header: __APPLE_USE_RFC_3542 must come
// before the first system header for IPV6_RECVPKTINFO/IPV6_PKTINFO on macOS.
#define __APPLE_USE_RFC_3542 1
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 // in6_pktinfo on Linux
#endif

#include "input/api/RemoteGamePad/RemoteGamePadUdp.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace RemoteGamePad::Net
{
	namespace
	{
		uint16_t port_of(const sockaddr_storage& a)
		{
			if (a.ss_family == AF_INET)
				return ntohs(((const sockaddr_in&)a).sin_port);
			if (a.ss_family == AF_INET6)
				return ntohs(((const sockaddr_in6&)a).sin6_port);
			return 0;
		}

		std::string addr_string(const sockaddr_storage& a)
		{
			char buf[INET6_ADDRSTRLEN] = {};
			if (a.ss_family == AF_INET)
				inet_ntop(AF_INET, &((const sockaddr_in&)a).sin_addr, buf, sizeof(buf));
			else if (a.ss_family == AF_INET6)
			{
				const auto& a6 = (const sockaddr_in6&)a;
				if (IN6_IS_ADDR_V4MAPPED(&a6.sin6_addr))
					inet_ntop(AF_INET, &a6.sin6_addr.s6_addr[12], buf, sizeof(buf));
				else
				{
					inet_ntop(AF_INET6, &a6.sin6_addr, buf, sizeof(buf));
					char ifname[IF_NAMESIZE] = {};
					if (a6.sin6_scope_id && if_indextoname(a6.sin6_scope_id, ifname))
						return std::string(buf) + "%" + ifname;
				}
			}
			return buf;
		}

		int open_socket(int family, uint16_t port, bool video)
		{
			const int fd = socket(family, SOCK_DGRAM, 0);
			if (fd < 0)
				return -1;
			int on = 1;
			if (family == AF_INET6)
			{
				setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
				setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on));
				sockaddr_in6 a{};
#ifdef __APPLE__
				a.sin6_len = sizeof(a);
#endif
				a.sin6_family = AF_INET6;
				a.sin6_port = htons(port);
				a.sin6_addr = in6addr_any;
				if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0)
				{
					close(fd);
					return -1;
				}
			}
			else
			{
#ifdef IP_RECVPKTINFO
				setsockopt(fd, IPPROTO_IP, IP_RECVPKTINFO, &on, sizeof(on)); // macOS
#else
				setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on)); // Linux
#endif
				sockaddr_in a{};
#ifdef __APPLE__
				a.sin_len = sizeof(a);
#endif
				a.sin_family = AF_INET;
				a.sin_port = htons(port);
				a.sin_addr.s_addr = htonl(INADDR_ANY);
				if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0)
				{
					const int e = errno;
					close(fd);
					errno = e;
					return -1;
				}
			}
			if (video)
			{
				int buf = 2 << 20;
				setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
				setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
#ifdef SO_NET_SERVICE_TYPE
				int st = NET_SERVICE_TYPE_VI; // Wi-Fi video access category
				setsockopt(fd, SOL_SOCKET, SO_NET_SERVICE_TYPE, &st, sizeof(st));
#endif
			}
			return fd;
		}
	}

	bool UdpAddr::SameRemote(const UdpAddr& o) const
	{
		if (remote.ss_family != o.remote.ss_family || port_of(remote) != port_of(o.remote))
			return false;
		if (remote.ss_family == AF_INET)
			return ((const sockaddr_in&)remote).sin_addr.s_addr == ((const sockaddr_in&)o.remote).sin_addr.s_addr;
		if (remote.ss_family == AF_INET6)
			return memcmp(&((const sockaddr_in6&)remote).sin6_addr, &((const sockaddr_in6&)o.remote).sin6_addr, 16) == 0;
		return false;
	}

	std::string UdpAddr::Address() const { return addr_string(remote); }
	std::string UdpAddr::Id() const { return Address() + ":" + std::to_string(port_of(remote)); }
	std::string UdpAddr::LocalAddress() const { return has_local ? addr_string(local) : std::string(); }

	bool UdpDual::Open(uint16_t port, std::string& error, bool video)
	{
		Close();
		m_fd4 = open_socket(AF_INET, port, video);
		if (m_fd4 < 0)
		{
			error = strerror(errno);
			return false;
		}
		m_fd6 = open_socket(AF_INET6, port, video); // optional
		return true;
	}

	void UdpDual::Close()
	{
		if (m_fd4 >= 0)
			close(m_fd4);
		if (m_fd6 >= 0)
			close(m_fd6);
		m_fd4 = m_fd6 = -1;
	}

	size_t UdpDual::ReceiveFrom(int fd, uint8_t* buffer, size_t capacity, UdpAddr& from)
	{
		from = {};
		iovec iov{buffer, capacity};
		alignas(cmsghdr) uint8_t control[256];
		msghdr msg{};
		msg.msg_name = &from.remote;
		msg.msg_namelen = sizeof(from.remote);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);
		const ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
		if (n <= 0)
			return 0;
		from.remote_len = msg.msg_namelen;
		for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
		{
			if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
			{
				in_pktinfo pi;
				memcpy(&pi, CMSG_DATA(c), sizeof(pi));
				auto& l = (sockaddr_in&)from.local;
#ifdef __APPLE__
				l.sin_len = sizeof(l);
#endif
				l.sin_family = AF_INET;
				l.sin_addr = pi.ipi_addr; // the destination address of the request
				from.ifindex = pi.ipi_ifindex;
				from.has_local = true;
			}
			else if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO)
			{
				in6_pktinfo pi;
				memcpy(&pi, CMSG_DATA(c), sizeof(pi));
				auto& l = (sockaddr_in6&)from.local;
#ifdef __APPLE__
				l.sin6_len = sizeof(l);
#endif
				l.sin6_family = AF_INET6;
				l.sin6_addr = pi.ipi6_addr;
				from.ifindex = pi.ipi6_ifindex;
				from.has_local = true;
			}
		}
		return (size_t)n;
	}

	size_t UdpDual::Receive(uint8_t* buffer, size_t capacity, UdpAddr& from, int timeout_ms)
	{
		if (m_fd4 < 0)
			return 0;
		pollfd p[2] = {{m_fd4, POLLIN, 0}, {m_fd6, POLLIN, 0}};
		const int count = m_fd6 >= 0 ? 2 : 1;
		if (poll(p, count, timeout_ms) <= 0)
			return 0;
		m_prefer6 = !m_prefer6;
		for (int k = 0; k < count; ++k)
		{
			const int i = (count == 2 && m_prefer6) ? 1 - k : k;
			if (p[i].revents & POLLIN)
				if (const size_t n = ReceiveFrom(p[i].fd, buffer, capacity, from))
					return n;
		}
		return 0;
	}

	bool UdpDual::Send(const UdpAddr& to, const uint8_t* data, size_t size) const
	{
		const bool v6 = to.remote.ss_family == AF_INET6;
		const int fd = v6 ? m_fd6 : m_fd4;
		if (fd < 0)
			return false;
		iovec iov{(void*)data, size};
		alignas(cmsghdr) uint8_t control[CMSG_SPACE(sizeof(in6_pktinfo))] = {};
		msghdr msg{};
		msg.msg_name = (void*)&to.remote;
		msg.msg_namelen = to.remote_len;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		if (to.has_local && to.local.ss_family == to.remote.ss_family)
		{
			msg.msg_control = control;
			cmsghdr* c = (cmsghdr*)control;
			if (v6)
			{
				in6_pktinfo pi{};
				pi.ipi6_addr = ((const sockaddr_in6&)to.local).sin6_addr;
				pi.ipi6_ifindex = to.ifindex;
				c->cmsg_level = IPPROTO_IPV6;
				c->cmsg_type = IPV6_PKTINFO;
				c->cmsg_len = CMSG_LEN(sizeof(pi));
				memcpy(CMSG_DATA(c), &pi, sizeof(pi));
				msg.msg_controllen = CMSG_SPACE(sizeof(pi));
			}
			else
			{
				in_pktinfo pi{};
				pi.ipi_spec_dst = ((const sockaddr_in&)to.local).sin_addr; // source address of the reply
				c->cmsg_level = IPPROTO_IP;
				c->cmsg_type = IP_PKTINFO;
				c->cmsg_len = CMSG_LEN(sizeof(pi));
				memcpy(CMSG_DATA(c), &pi, sizeof(pi));
				msg.msg_controllen = CMSG_SPACE(sizeof(pi));
			}
		}
		return sendmsg(fd, &msg, 0) == (ssize_t)size;
	}
}
