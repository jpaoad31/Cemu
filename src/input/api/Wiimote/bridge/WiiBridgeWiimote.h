#pragma once

// Wii BT Bridge backend for real Wii devices (Wii Remote, Wii Balance Board).
//
// The bridge (github.com/jpaoad31/wii-bt-bridge: a Pico W on a USB network, or a Raspberry Pi)
// does the Bluetooth part, including the Wii's binary PIN that macOS / iOS can't supply, and
// forwards the devices' raw HID reports over UDP (default 192.168.7.1:4730). Protocol: the
// bridge repo's docs/protocol.md. Reports are passed through unchanged, so everything above
// WiimoteDevice (extension init, Balance Board calibration, report 0x34, ...) works as with
// hidapi or IOBluetooth.
//
// One session (UDP socket + thread) talks to the bridge; every connected bridge slot is a
// WiiBridgeWiimote. Devices are identified by Bluetooth address, which stays the same when a
// device reconnects (possibly in another slot).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "input/api/Wiimote/WiimoteDevice.h"

struct WiiBridgeDeviceState;

class WiiBridgeWiimote : public WiimoteDevice
{
public:
	explicit WiiBridgeWiimote(std::shared_ptr<WiiBridgeDeviceState> state);

	// data starts with the report ID: sent as the bridge's DATA_OUT (the bridge adds the 0xA2 header)
	bool write_data(const std::vector<uint8>& data) override;
	// a report starting with its ID (the bridge removed the 0xA1 header), an empty vector when none
	// is waiting, nullopt once the device disconnected from the bridge
	std::optional<std::vector<uint8>> read_data() override;
	// same Bluetooth address = same device
	bool operator==(const WiimoteDevice& o) const override;
	std::string get_product_name() const override;
	std::string get_location() const override;
	std::string get_identity() const override { return "bridge " + get_address(); }

	std::string get_address() const;

	// the devices currently connected to the bridge (starts / stops / reconfigures the session from
	// the settings and the CEMU_WII_BRIDGE environment variable first)
	static std::vector<WiimoteDevicePtr> get_devices();

private:
	std::shared_ptr<WiiBridgeDeviceState> m_state;
};

namespace WiiBridge
{
	// re-read the settings: GetConfig().wii_bridge, overridden by CEMU_WII_BRIDGE=host[:port]
	// (which also enables the bridge). Starts or stops the session as needed.
	void apply_settings();
	// ask the bridge to look for devices in sync mode (red SYNC button / 1+2) for this long
	void start_scan(int seconds = 30);
	void stop_scan();

	struct Status
	{
		bool enabled = false;
		bool from_environment = false;   // CEMU_WII_BRIDGE is set
		std::string endpoint;            // "192.168.7.1:4730"
		bool bridge_answering = false;   // HELLO_ACK within the last 3 s
		bool busy = false;               // the bridge answers BUSY: another program owns it
		std::string busy_owner;          // that program's address, "192.168.7.2:62657"
		int firmware_version = 0;
		std::string bridge_address;      // the bridge's own Bluetooth address
		bool scanning = false;
		std::vector<std::string> devices; // "slot 3: Nintendo RVL-WBC-01 (34:AF:2C:E6:C1:87)"
		std::string last_log;            // the bridge's last LOG line
	};
	Status get_status();
}
