#pragma once

#include <api/Wiimote/WiimoteDevice.h>

class HidapiWiimote : public WiimoteDevice {
public:
    HidapiWiimote(struct SDL_hid_device* dev, std::string_view path, std::string_view identity, std::string_view product_name);
    ~HidapiWiimote() override;

    bool write_data(const std::vector<uint8> &data) override;
    std::optional<std::vector<uint8>> read_data() override;
    bool operator==(const WiimoteDevice& o) const override;
    std::string get_product_name() const override { return m_product_name; }
    std::string get_location() const override { return m_path; }
    std::string get_identity() const override { return m_identity; }

    static std::vector<WiimoteDevicePtr> get_devices();

private:
    struct SDL_hid_device* m_handle;
    const std::string m_path;
    const std::string m_identity;
    const std::string m_product_name;

};
