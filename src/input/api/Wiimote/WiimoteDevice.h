#pragma once

class WiimoteDevice
{
	friend class WiimoteInfo;
public:
	virtual ~WiimoteDevice() = default;

	virtual bool write_data(const std::vector<uint8>& data) = 0;
	virtual std::optional<std::vector<uint8_t>> read_data() = 0;

	virtual bool operator==(const WiimoteDevice& o) const = 0;

	// what stays the same when this device disconnects and reconnects (e.g. its Bluetooth address),
	// so it can get its previous index back; empty if the backend can't tell
	virtual std::string get_identity() const { return {}; }
};

using WiimoteDevicePtr = std::shared_ptr<WiimoteDevice>;
