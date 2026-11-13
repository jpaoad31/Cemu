#pragma once

// macOS IOBluetooth backend for real Wii devices (Wii Remote, Wii Balance Board).
// Connects by Bluetooth address with direct L2CAP channels (PSM 0x11 control, 0x13 interrupt),
// without macOS pairing. See IOBluetoothWiimote.mm for the credits.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "input/api/Wiimote/WiimoteDevice.h"

struct IOBluetoothWiimoteConnection;

class IOBluetoothWiimote : public WiimoteDevice
{
public:
	explicit IOBluetoothWiimote(std::shared_ptr<IOBluetoothWiimoteConnection> connection);
	~IOBluetoothWiimote() override;

	// data starts with the report ID (the 0xA2 / 0xA1 HID transaction byte is added / removed here)
	bool write_data(const std::vector<uint8>& data) override;
	// nullopt once the device disconnected, an empty vector when no report is waiting
	std::optional<std::vector<uint8>> read_data() override;
	bool operator==(const WiimoteDevice& o) const override;
	std::string get_product_name() const override;
	std::string get_location() const override;

	std::string get_address() const;

	// the devices currently connected through IOBluetooth (they stay open until they disconnect)
	static std::vector<WiimoteDevicePtr> get_devices();
	// true if a device with this Bluetooth address is connected (or connecting) through IOBluetooth;
	// any format ("34-af-2c-e6-c1-87", "34:AF:2C:E6:C1:87", ...)
	static bool is_handling_address(std::string_view address);

	// on-demand device inquiry: runs up to timeout_seconds, ends as soon as a Nintendo RVL device
	// ("Nintendo RVL-CNT-01*", "Nintendo RVL-WBC-01") is found and connected, or on cancel_scan()
	static void start_scan(int timeout_seconds = 60);
	static void cancel_scan();
	// connect a device by address without an inquiry (it must be in sync mode or reconnecting)
	static void connect_address(const std::string& address);
	static void disconnect_all();

	struct ScanStatus
	{
		bool scanning = false;
		int seconds_left = 0;
		std::string text;                       // last scan state / result, for the UI
		std::vector<std::string> connected;     // "Nintendo RVL-WBC-01 (34-af-2c-e6-c1-87)"
	};
	static ScanStatus get_status();

private:
	std::shared_ptr<IOBluetoothWiimoteConnection> m_connection;
};
