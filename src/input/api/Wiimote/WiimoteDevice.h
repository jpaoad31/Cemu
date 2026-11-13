#pragma once

class WiimoteDevice
{
	friend class WiimoteInfo;
public:
	virtual ~WiimoteDevice() = default;

	virtual bool write_data(const std::vector<uint8>& data) = 0;
	virtual std::optional<std::vector<uint8_t>> read_data() = 0;

	virtual bool operator==(const WiimoteDevice& o) const = 0;

	// HID product name if the backend knows it ("Nintendo RVL-CNT-01", "Nintendo RVL-WBC-01", ...)
	virtual std::string get_product_name() const { return {}; }
	// where the device is (HID path or Bluetooth address), for logging
	virtual std::string get_location() const { return {}; }
	// what stays the same when this device reconnects (used to give it its previous index back)
	virtual std::string get_identity() const { return get_location(); }
};

using WiimoteDevicePtr = std::shared_ptr<WiimoteDevice>;
