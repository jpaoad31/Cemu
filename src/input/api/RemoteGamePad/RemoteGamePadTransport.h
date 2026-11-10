#pragma once

// The Remote GamePad provider receives protocol packets through this interface, so another
// transport (planned: Bluetooth LE, the Mac as central, controls only) can be added without
// touching the input, touch or motion code. Each transport delivers whole protocol packets.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

class RemoteGamePadTransport
{
public:
	struct Peer
	{
		std::string id;      // unique per sender, e.g. "192.168.1.20:50123"
		std::string address; // for display, e.g. "192.168.1.20"
		bool operator==(const Peer& o) const { return id == o.id; }
		bool operator!=(const Peer& o) const { return id != o.id; }
	};

	virtual ~RemoteGamePadTransport() = default;

	// open/bind; on failure returns false with a message in error
	virtual bool open(std::string& error) = 0;
	virtual bool is_open() const = 0;
	virtual void close() = 0;
	// waits up to timeout for one packet; returns its size, 0 on timeout or error
	virtual size_t receive(uint8_t* buffer, size_t capacity, Peer& from, std::chrono::milliseconds timeout) = 0;
	virtual bool send(const Peer& to, const uint8_t* data, size_t size) = 0;
	// e.g. "UDP port 26770"
	virtual std::string description() const = 0;
};

std::unique_ptr<RemoteGamePadTransport> CreateRemoteGamePadUDPTransport(uint16_t port);
