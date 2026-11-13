#include "HidapiWiimote.h"
#include <SDL3/SDL.h>
#include <cwchar>
#ifdef HAS_IOBLUETOOTH
#include "input/api/Wiimote/darwin/IOBluetoothWiimote.h"
#endif

static constexpr uint16 WIIMOTE_VENDOR_ID = 0x057e;
static constexpr uint16 WIIMOTE_PRODUCT_ID = 0x0306;
static constexpr uint16 WIIMOTE_MP_PRODUCT_ID = 0x0330;
static constexpr uint16 WIIMOTE_MAX_INPUT_REPORT_LENGTH = 22;
static constexpr auto PRO_CONTROLLER_NAME = L"Nintendo RVL-CNT-01-UC";

HidapiWiimote::HidapiWiimote(SDL_hid_device* dev, std::string_view path, std::string_view identity, std::string_view product_name)
 : m_handle(dev), m_path(path), m_identity(identity), m_product_name(product_name) {

}

bool HidapiWiimote::write_data(const std::vector<uint8> &data) {
    return SDL_hid_write(m_handle, data.data(), data.size()) >= 0;
}

std::optional<std::vector<uint8>> HidapiWiimote::read_data() {
    std::array<uint8, WIIMOTE_MAX_INPUT_REPORT_LENGTH> read_data{};
    const auto result = SDL_hid_read(m_handle, read_data.data(), WIIMOTE_MAX_INPUT_REPORT_LENGTH);
    if (result < 0)
        return {};
    return {{read_data.cbegin(), read_data.cbegin() + result}};
}

std::vector<WiimoteDevicePtr> HidapiWiimote::get_devices() {
    std::vector<WiimoteDevicePtr> wiimote_devices;
    SDL_hid_init();
    const auto device_enumeration = SDL_hid_enumerate(WIIMOTE_VENDOR_ID, 0x0);

    for (auto it = device_enumeration; it != nullptr; it = it->next){
        if (it->product_id != WIIMOTE_PRODUCT_ID && it->product_id != WIIMOTE_MP_PRODUCT_ID)
            continue;
        if (it->product_string && std::wcscmp(it->product_string, PRO_CONTROLLER_NAME) == 0)
            continue;
#ifdef HAS_IOBLUETOOTH
        // already connected directly through IOBluetooth: macOS reports the Bluetooth address as
        // the HID serial number, so the same device isn't opened twice
        if (it->serial_number && *it->serial_number)
        {
            std::string serial;
            for (const wchar_t* c = it->serial_number; *c; ++c)
                serial.push_back(*c < 0x80 ? (char)*c : '?');
            if (IOBluetoothWiimote::is_handling_address(serial))
            {
                static std::string s_logged;
                if (s_logged != serial)
                {
                    s_logged = serial;
                    cemuLog_log(LogType::InputAPI, "Wiimote: hidapi device {} is already connected through IOBluetooth, skipping it", serial);
                }
                continue;
            }
        }
#endif
        auto dev = SDL_hid_open_path(it->path);
        if (!dev){
            cemuLog_logDebug(LogType::Force, "Unable to open Wiimote device at {}: {}", it->path, SDL_GetError());
        }
        else {
            SDL_hid_set_nonblocking(dev, true);
            // a Balance Board is a Wii Remote too (same VID/PID), named "Nintendo RVL-WBC-01"
            std::string product_name;
            if (it->product_string)
            {
                for (const wchar_t* c = it->product_string; *c; ++c)
                    product_name.push_back(*c < 0x80 ? (char)*c : '?');
            }
            // the path can change when a device reconnects; the serial number (the Bluetooth
            // address for Bluetooth HID devices on macOS and Windows) doesn't
            std::string identity;
            if (it->serial_number)
            {
                for (const wchar_t* c = it->serial_number; *c; ++c)
                    identity.push_back(*c < 0x80 ? (char)*c : '?');
            }
            if (identity.empty())
                identity = it->path;
            wiimote_devices.push_back(std::make_shared<HidapiWiimote>(dev, it->path, identity, product_name));
        }
    }
    SDL_hid_free_enumeration(device_enumeration);
    return wiimote_devices;
}

bool HidapiWiimote::operator==(const WiimoteDevice& rhs) const  {
    auto other = dynamic_cast<const HidapiWiimote*>(&rhs);
	if (!other)
		return false;
	return m_path == other->m_path;
}

HidapiWiimote::~HidapiWiimote() {
    SDL_hid_close(m_handle);
}
