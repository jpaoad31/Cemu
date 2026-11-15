#include "wxgui/input/panels/WiiBridgePanel.h"

#ifdef SUPPORTS_WIIMOTE

#include <wx/sizer.h>
#include <wx/checkbox.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include "input/api/Wiimote/bridge/WiiBridgeWiimote.h"
#include "config/CemuConfig.h"

WiiBridgePanel::WiiBridgePanel(wxWindow* parent)
	: wxPanel(parent)
{
	const auto& config = GetConfig();
	auto* main_sizer = new wxBoxSizer(wxVERTICAL);

	auto* row = new wxBoxSizer(wxHORIZONTAL);
	m_enabled = new wxCheckBox(this, wxID_ANY, _("Wii BT Bridge at"));
	m_enabled->SetValue(config.wii_bridge.enabled);
	m_enabled->SetToolTip(_("Use a Wii BT Bridge (a Pico W or Raspberry Pi that connects real Wii Remotes and Balance Boards over Bluetooth and forwards them over the network). A Pico W on USB is at 192.168.7.1:4730. One setting for all players."));
	m_enabled->Bind(wxEVT_CHECKBOX, &WiiBridgePanel::on_settings_changed, this);
	row->Add(m_enabled, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

	m_address = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(fmt::format("{}:{}", config.wii_bridge.host.GetValue(), config.wii_bridge.port.GetValue())),
		wxDefaultPosition, wxSize(150, -1), wxTE_PROCESS_ENTER);
	// Saved on Enter, the checkbox and "Scan with bridge" -- not on losing focus: on macOS the
	// focused text box loses focus while the dialog is being destroyed, after this panel is gone.
	m_address->Bind(wxEVT_TEXT_ENTER, &WiiBridgePanel::on_settings_changed, this);
	row->Add(m_address, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

	m_scan = new wxButton(this, wxID_ANY, _("Scan with bridge"));
	m_scan->SetToolTip(_("The bridge looks for Wii devices for 30 s: press the red SYNC button on the Wii Remote or the board. A device synced with the bridge before reconnects with any button (the power button on the board) without a scan."));
	m_scan->Bind(wxEVT_BUTTON, &WiiBridgePanel::on_scan, this);
	row->Add(m_scan, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);

	m_stop = new wxButton(this, wxID_ANY, _("Stop"));
	m_stop->Bind(wxEVT_BUTTON, &WiiBridgePanel::on_stop, this);
	row->Add(m_stop, 0, wxALIGN_CENTER_VERTICAL);
	main_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, 5);

	m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
	main_sizer->Add(m_status, 0, wxEXPAND);

	SetSizer(main_sizer);

	m_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, &WiiBridgePanel::on_timer, this, m_timer->GetId());
	m_timer->Start(250);
}

WiiBridgePanel::~WiiBridgePanel()
{
	if (m_timer)
		m_timer->Stop();
}

void WiiBridgePanel::on_settings_changed(wxCommandEvent& event)
{
	if (IsBeingDeleted() || !m_address || !m_enabled)
		return;
	auto& config = GetConfig();
	std::string address = m_address->GetValue().ToStdString();
	std::string host = address;
	uint16 port = 4730;
	if (const auto colon = address.rfind(':'); colon != std::string::npos)
	{
		host = address.substr(0, colon);
		const auto parsed = std::strtoul(address.c_str() + colon + 1, nullptr, 10);
		if (parsed > 0 && parsed <= 0xFFFF)
			port = (uint16)parsed;
	}
	if (host.empty())
	{
		host = "192.168.7.1";   // a Pico W bridge on USB
		m_address->ChangeValue(wxString::FromUTF8(fmt::format("{}:{}", host, port)));
	}
	const bool enabled = m_enabled->GetValue();
	if (config.wii_bridge.enabled == enabled && config.wii_bridge.host.GetValue() == host && config.wii_bridge.port == port)
		return;
	config.wii_bridge.enabled = enabled;
	config.wii_bridge.host = host;
	config.wii_bridge.port = port;
	GetConfigHandle().Save();
	WiiBridge::apply_settings();
}

void WiiBridgePanel::on_scan(wxCommandEvent& event)
{
	m_enabled->SetValue(true);
	wxCommandEvent dummy;
	on_settings_changed(dummy);   // also picks up an edited address
	WiiBridge::start_scan(30);
}

void WiiBridgePanel::on_stop(wxCommandEvent& event)
{
	WiiBridge::stop_scan();
}

void WiiBridgePanel::on_timer(wxTimerEvent& event)
{
	const auto status = WiiBridge::get_status();
	// another panel (the other controller type) may have changed the shared settings
	if (!m_address->HasFocus() && m_enabled->GetValue() != status.enabled && !status.from_environment)
		m_enabled->SetValue(status.enabled);

	wxString label;
	if (!status.enabled)
		label = _("Bridge off.");
	else if (status.busy)
		label = wxString::Format(_("The bridge at %s is in use by another program (%s). Close it (e.g. another emulator) to use the bridge here."),
			wxString::FromUTF8(status.endpoint), wxString::FromUTF8(status.busy_owner));
	else if (!status.bridge_answering)
		label = wxString::Format(_("No answer from the bridge at %s (plugged in? the USB network up?)"), wxString::FromUTF8(status.endpoint));
	else
	{
		label = wxString::Format(_("Bridge %s (firmware %d)%s."), wxString::FromUTF8(status.bridge_address), status.firmware_version,
			status.scanning ? _(", scanning: press SYNC on the device") : wxString());
		if (status.devices.empty())
			label += _(" No devices connected.");
		else
		{
			wxString list;
			for (const auto& d : status.devices)
				list += (list.empty() ? "" : ", ") + wxString::FromUTF8(d);
			label += wxString::Format(_(" Connected: %s. Add it above with the + button, API \"Wiimote\"."), list);
		}
	}
	if (status.from_environment)
		label += _(" [CEMU_WII_BRIDGE overrides the settings]");
	m_scan->Enable(!status.scanning);
	m_stop->Enable(status.scanning);
	if (m_status->GetLabel() != label)
	{
		m_status->SetLabel(label);
		m_status->Wrap(600);
		GetParent()->Layout();
	}
}

#endif // SUPPORTS_WIIMOTE
