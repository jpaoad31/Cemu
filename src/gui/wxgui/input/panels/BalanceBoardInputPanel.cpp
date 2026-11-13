#include "wxgui/input/panels/BalanceBoardInputPanel.h"

#include <wx/gbsizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/slider.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <wx/checkbox.h>

#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/components/wxInputDraw.h"

#ifdef HAS_IOBLUETOOTH
#include "input/api/Wiimote/darwin/IOBluetoothWiimote.h"
#endif
#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/bridge/WiiBridgeWiimote.h"
#include "config/CemuConfig.h"
#endif

BalanceBoardInputPanel::BalanceBoardInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	auto bold_font = GetFont();
	bold_font.MakeBold();

	auto* main_sizer = new wxBoxSizer(wxVERTICAL);

	auto* info = new wxStaticText(this, wxID_ANY, _("The Balance Board always uses Wii Remote channel 4 (WPAD channel 3). Use a Wiimote on another controller for the menus.\nReal board: connect it (through a Wii BT Bridge, below on macOS, or pair it in the Bluetooth settings), then add it here with API \"Wiimote\". With a Wiimote controller added, the real board is the source and the options below are unused."));
	info->Wrap(600);
	main_sizer->Add(info, 0, wxALL, 5);

#ifdef HAS_IOBLUETOOTH
	{
		auto* bt_sizer = new wxBoxSizer(wxHORIZONTAL);
		m_bt_connect = new wxButton(this, wxID_ANY, _("Connect real Wii device"));
		m_bt_connect->SetToolTip(_("Click, then press the red sync button in the board's battery compartment (or 1+2 on a Wii Remote). Scans up to 60 s and connects the first Nintendo RVL device found, without macOS pairing."));
		m_bt_connect->Bind(wxEVT_BUTTON, &BalanceBoardInputPanel::on_bt_connect, this);
		bt_sizer->Add(m_bt_connect, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		m_bt_cancel = new wxButton(this, wxID_ANY, _("Cancel"));
		m_bt_cancel->Bind(wxEVT_BUTTON, &BalanceBoardInputPanel::on_bt_cancel, this);
		m_bt_cancel->Disable();
		bt_sizer->Add(m_bt_cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
		m_bt_status = new wxStaticText(this, wxID_ANY, _("Click, then press the red sync button (up to 60 s)."));
		bt_sizer->Add(m_bt_status, 1, wxALIGN_CENTER_VERTICAL);
		main_sizer->Add(bt_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

		m_bt_timer = new wxTimer(this);
		Bind(wxEVT_TIMER, &BalanceBoardInputPanel::on_bt_timer, this, m_bt_timer->GetId());
		m_bt_timer->Start(250);
	}
#endif
#ifdef SUPPORTS_WIIMOTE
	{
		const auto& config = GetConfig();
		auto* bridge_sizer = new wxBoxSizer(wxHORIZONTAL);
		m_bridge_enabled = new wxCheckBox(this, wxID_ANY, _("Wii BT Bridge at"));
		m_bridge_enabled->SetValue(config.wii_bridge.enabled);
		m_bridge_enabled->SetToolTip(_("Use a Wii BT Bridge (a Pico W or Raspberry Pi that connects real Wii Remotes and Balance Boards over Bluetooth and forwards them over the network). A Pico W on USB is at 192.168.7.1:4730."));
		m_bridge_enabled->Bind(wxEVT_CHECKBOX, &BalanceBoardInputPanel::on_bridge_settings_changed, this);
		bridge_sizer->Add(m_bridge_enabled, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		m_bridge_address = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(fmt::format("{}:{}", config.wii_bridge.host.GetValue(), config.wii_bridge.port.GetValue())),
			wxDefaultPosition, wxSize(150, -1), wxTE_PROCESS_ENTER);
		// Saved on Enter, the checkbox and "Scan with bridge" -- not on losing focus: on macOS the
		// focused text box loses focus while the dialog is being destroyed, after this panel is gone.
		m_bridge_address->Bind(wxEVT_TEXT_ENTER, &BalanceBoardInputPanel::on_bridge_settings_changed, this);
		bridge_sizer->Add(m_bridge_address, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		m_bridge_scan = new wxButton(this, wxID_ANY, _("Scan with bridge"));
		m_bridge_scan->SetToolTip(_("The bridge looks for Wii devices for 30 s: press the red sync button on the board (or 1+2 on a Wii Remote). A device synced with the bridge before reconnects with its power button (any button on a Wii Remote) without a scan."));
		m_bridge_scan->Bind(wxEVT_BUTTON, &BalanceBoardInputPanel::on_bridge_scan, this);
		bridge_sizer->Add(m_bridge_scan, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
		m_bridge_stop = new wxButton(this, wxID_ANY, _("Stop"));
		m_bridge_stop->Bind(wxEVT_BUTTON, &BalanceBoardInputPanel::on_bridge_stop, this);
		bridge_sizer->Add(m_bridge_stop, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
		main_sizer->Add(bridge_sizer, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);
		m_bridge_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
		main_sizer->Add(m_bridge_status, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

		m_bridge_timer = new wxTimer(this);
		Bind(wxEVT_TIMER, &BalanceBoardInputPanel::on_bridge_timer, this, m_bridge_timer->GetId());
		m_bridge_timer->Start(250);
	}
#endif
	main_sizer->Add(new wxStaticLine(this), 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 5);

	m_item_sizer = new wxGridBagSizer();

	// buttons and hotkeys
	sint32 row = 0;
	sint32 column = 0;
	auto* text = new wxStaticText(this, wxID_ANY, _("Buttons"));
	text->SetFont(bold_font);
	m_item_sizer->Add(text, wxGBPosition(row, column), wxGBSpan(1, 2), wxALL | wxEXPAND, 5);
	add_button_row(++row, column, BalanceBoardController::kButtonId_Power);

	row++;
	text = new wxStaticText(this, wxID_ANY, _("Hotkeys"));
	text->SetFont(bold_font);
	m_item_sizer->Add(text, wxGBPosition(++row, column), wxGBSpan(1, 2), wxALL | wxEXPAND, 5);
	add_button_row(++row, column, BalanceBoardController::kButtonId_StepOffToggle);
	add_button_row(++row, column, BalanceBoardController::kButtonId_StepOffHold);

	m_item_sizer->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVERTICAL), wxGBPosition(0, column + 2), wxGBSpan(8, 1), wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

	// balance stick
	row = 0;
	column += 3;
	text = new wxStaticText(this, wxID_ANY, _("Balance"));
	text->SetFont(bold_font);
	m_item_sizer->Add(text, wxGBPosition(row, column), wxGBSpan(1, 2), wxALL | wxEXPAND, 5);
	for (const auto id : {BalanceBoardController::kButtonId_Balance_Up, BalanceBoardController::kButtonId_Balance_Down,
		BalanceBoardController::kButtonId_Balance_Left, BalanceBoardController::kButtonId_Balance_Right})
	{
		add_button_row(++row, column, id);
	}
	m_draw = new wxInputDraw(this, wxID_ANY, wxDefaultPosition, {60, 60});
	m_item_sizer->Add(m_draw, wxGBPosition(++row, column + 1), wxGBSpan(2, 1), wxTOP | wxBOTTOM | wxALIGN_CENTER, 5);

	m_item_sizer->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVERTICAL), wxGBPosition(0, column + 2), wxGBSpan(8, 1), wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

	// options
	row = 0;
	column += 3;
	text = new wxStaticText(this, wxID_ANY, _("Options"));
	text->SetFont(bold_font);
	m_item_sizer->Add(text, wxGBPosition(row, column), wxGBSpan(1, 3), wxALL | wxEXPAND, 5);
	m_weight = add_setting_row(++row, column, _("Weight"), 0, (int)BalanceBoardMath::kMaxWeight, m_weight_text);
	m_foot_separation = add_setting_row(++row, column, _("Foot separation"), 0, 50, m_foot_separation_text);
	m_foot_length = add_setting_row(++row, column, _("Foot length"), 0, 40, m_foot_length_text);
	m_battery = add_setting_row(++row, column, _("Battery"), 0, 100, m_battery_text);

	m_weight->SetValue((int)BalanceBoardMath::kDefaultWeight);
	m_foot_separation->SetValue((int)BalanceBoardMath::kDefaultFootSeparation);
	m_foot_length->SetValue((int)BalanceBoardMath::kDefaultFootLength);
	m_battery->SetValue(BalanceBoardMath::kDefaultBattery);
	update_setting_texts();

	main_sizer->Add(m_item_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

	m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
	main_sizer->Add(m_status, 0, wxALL | wxEXPAND, 5);

	SetSizer(main_sizer);
	Layout();
}

void BalanceBoardInputPanel::add_button_row(sint32 row, sint32 column, BalanceBoardController::ButtonId button_id)
{
	m_item_sizer->Add(
		new wxStaticText(this, wxID_ANY, wxGetTranslation(wxString::FromUTF8(BalanceBoardController::get_button_name(button_id)))),
		wxGBPosition(row, column),
		wxDefaultSpan,
		wxALL | wxALIGN_CENTER_VERTICAL, 5);

	auto* text_ctrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxTE_PROCESS_ENTER | wxTE_PROCESS_TAB);
	text_ctrl->SetClientData((void*)button_id);
	text_ctrl->SetMinSize(wxSize(150, -1));
	text_ctrl->SetEditable(false);
	text_ctrl->SetBackgroundColour(kKeyColourNormalMode);
	bind_hotkey_events(text_ctrl);
	m_item_sizer->Add(text_ctrl, wxGBPosition(row, column + 1), wxDefaultSpan, wxALL | wxEXPAND, 5);
}

wxSlider* BalanceBoardInputPanel::add_setting_row(sint32 row, sint32 column, const wxString& name, int min, int max, wxStaticText*& value_text)
{
	m_item_sizer->Add(new wxStaticText(this, wxID_ANY, name), wxGBPosition(row, column), wxDefaultSpan, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	auto* slider = new wxSlider(this, wxID_ANY, min, min, max);
	slider->SetMinSize(wxSize(150, -1));
	slider->Bind(wxEVT_SLIDER, &BalanceBoardInputPanel::on_setting_changed, this);
	m_item_sizer->Add(slider, wxGBPosition(row, column + 1), wxDefaultSpan, wxTOP | wxBOTTOM | wxEXPAND, 5);

	value_text = new wxStaticText(this, wxID_ANY, wxEmptyString);
	value_text->SetMinSize(wxSize(60, -1));
	m_item_sizer->Add(value_text, wxGBPosition(row, column + 2), wxDefaultSpan, wxALL | wxALIGN_CENTER_VERTICAL, 5);
	return slider;
}

void BalanceBoardInputPanel::update_setting_texts()
{
	m_weight_text->SetLabel(wxString::Format("%d kg", m_weight->GetValue()));
	m_foot_separation_text->SetLabel(wxString::Format("%d cm", m_foot_separation->GetValue()));
	m_foot_length_text->SetLabel(wxString::Format("%d cm", m_foot_length->GetValue()));
	m_battery_text->SetLabel(wxString::Format("%d%%", m_battery->GetValue()));
}

void BalanceBoardInputPanel::on_setting_changed(wxCommandEvent& event)
{
	update_setting_texts();
	if (const auto board = m_board.lock())
	{
		board->set_weight(m_weight->GetValue());
		board->set_foot_separation(m_foot_separation->GetValue());
		board->set_foot_length(m_foot_length->GetValue());
		board->set_battery(m_battery->GetValue());
	}
}

BalanceBoardInputPanel::~BalanceBoardInputPanel()
{
#ifdef HAS_IOBLUETOOTH
	if (m_bt_timer)
		m_bt_timer->Stop();
#endif
#ifdef SUPPORTS_WIIMOTE
	if (m_bridge_timer)
		m_bridge_timer->Stop();
#endif
}

#ifdef SUPPORTS_WIIMOTE
void BalanceBoardInputPanel::on_bridge_settings_changed(wxCommandEvent& event)
{
	if (IsBeingDeleted() || !m_bridge_address || !m_bridge_enabled)
		return;
	auto& config = GetConfig();
	std::string address = m_bridge_address->GetValue().ToStdString();
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
		m_bridge_address->ChangeValue(wxString::FromUTF8(fmt::format("{}:{}", host, port)));
	}
	const bool enabled = m_bridge_enabled->GetValue();
	if (config.wii_bridge.enabled == enabled && config.wii_bridge.host.GetValue() == host && config.wii_bridge.port == port)
		return;
	config.wii_bridge.enabled = enabled;
	config.wii_bridge.host = host;
	config.wii_bridge.port = port;
	GetConfigHandle().Save();
	WiiBridge::apply_settings();
}

void BalanceBoardInputPanel::on_bridge_scan(wxCommandEvent& event)
{
	m_bridge_enabled->SetValue(true);
	wxCommandEvent dummy;
	on_bridge_settings_changed(dummy);   // also picks up an edited address
	WiiBridge::start_scan(30);
}

void BalanceBoardInputPanel::on_bridge_stop(wxCommandEvent& event)
{
	WiiBridge::stop_scan();
}

void BalanceBoardInputPanel::on_bridge_timer(wxTimerEvent& event)
{
	const auto status = WiiBridge::get_status();
	wxString label;
	if (!status.enabled)
		label = _("Bridge off.");
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
			label += wxString::Format(_(" Connected: %s. Add it above with API \"Wiimote\" (refresh the list)."), list);
		}
	}
	if (status.from_environment)
		label += _(" [CEMU_WII_BRIDGE overrides the settings]");
	m_bridge_scan->Enable(!status.scanning);
	m_bridge_stop->Enable(status.scanning);
	if (m_bridge_status->GetLabel() != label)
	{
		m_bridge_status->SetLabel(label);
		m_bridge_status->Wrap(600);
		Layout();
	}
}
#endif

#ifdef HAS_IOBLUETOOTH
void BalanceBoardInputPanel::on_bt_connect(wxCommandEvent& event)
{
	IOBluetoothWiimote::start_scan(60);
}

void BalanceBoardInputPanel::on_bt_cancel(wxCommandEvent& event)
{
	IOBluetoothWiimote::cancel_scan();
}

void BalanceBoardInputPanel::on_bt_timer(wxTimerEvent& event)
{
	const auto status = IOBluetoothWiimote::get_status();
	wxString label;
	if (status.scanning)
		label = wxString::Format(_("Scanning... %d s left. %s"), status.seconds_left, wxString::FromUTF8(status.text));
	else if (!status.text.empty())
		label = wxString::FromUTF8(status.text);
	else
		label = _("Click, then press the red sync button (up to 60 s).");
	if (!status.connected.empty())
	{
		wxString list;
		for (const auto& c : status.connected)
			list += (list.empty() ? "" : ", ") + wxString::FromUTF8(c);
		label += wxString::Format(_("\nConnected over Bluetooth: %s. Add it above with API \"Wiimote\" (refresh the list)."), list);
	}
	if (m_bt_connect->IsEnabled() == status.scanning)
		m_bt_connect->Enable(!status.scanning);
	if (m_bt_cancel->IsEnabled() != status.scanning)
		m_bt_cancel->Enable(status.scanning);
	if (m_bt_status->GetLabel() != label)
	{
		const bool relayout = m_bt_status->GetLabel().Freq('\n') != label.Freq('\n');
		m_bt_status->SetLabel(label);
		if (relayout)
			Layout();
	}
}
#endif

void BalanceBoardInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	InputPanel::on_timer(emulated_controller, controller);

	const auto board = std::dynamic_pointer_cast<BalanceBoardController>(emulated_controller);
	if (!board)
		return;

	wxString label;
	const auto real = board->get_real_board();
	if (real.assigned)
	{
		// a real board is the source: its own load cells, converted with its own calibration
		wxString state;
		if (!real.connected)
			state = _("not connected (connect or pair it, then wait a few seconds)");
		else if (!real.is_board)
			state = _("connected, but this Wii Remote is not a Balance Board");
		else if (!real.calibrated)
			state = _("connected, reading its calibration...");
		else if (!real.has_data)
			state = _("connected, waiting for its first report...");
		else
			state = real.crc_ok ? _("ready, calibration CRC ok") : _("ready, but the calibration CRC does NOT match");
		label = wxString::Format(_("Source: real Balance Board (Wiimote %d): %s"), (int)real.device_index + 1, state);

		if (real.calibrated && real.has_data)
		{
			const auto cal = BalanceBoardMath::ParseCalibration(real.data.memory);
			double kg[BalanceBoardMath::kSensorCount];
			std::array<double, BalanceBoardMath::kSensorCount> sensors;
			for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
				sensors[i] = kg[i] = BalanceBoardMath::RawToKg(cal, i, real.data.raw[i], cal.raw[i][0]);
			double x, y;
			BalanceBoardMath::SensorsToCenterOfBalance(sensors, x, y);
			m_draw->SetAxisValue({(float)x, (float)y});
			label += wxString::Format(_("\nTotal %.1f kg (TL %.1f  TR %.1f  BL %.1f  BR %.1f)  Temperature %d  Battery 0x%02x (level %d/4)%s"),
				kg[0] + kg[1] + kg[2] + kg[3], kg[BalanceBoardMath::kTopLeft], kg[BalanceBoardMath::kTopRight],
				kg[BalanceBoardMath::kBottomLeft], kg[BalanceBoardMath::kBottomRight], (int)(sint8)real.data.temperature,
				real.data.battery, BalanceBoardMath::BatteryRawToLevel(real.data.battery), (board->is_a_down() ? _("  Power pressed") : wxString()));
		}
		else
			m_draw->SetAxisValue({});
	}
	else
	{
		m_draw->SetAxisValue(board->get_axis());

		const auto raw = board->get_sensor_values();
		double kg[BalanceBoardMath::kSensorCount];
		for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
			kg[i] = BalanceBoardMath::RawToKg(raw[i]);
		label = wxString::Format(_("Source: emulated. %s  Total %.1f kg (TL %.1f  TR %.1f  BL %.1f  BR %.1f)"),
			board->is_stepped_off() ? _("Stepped off.") : _("On the board."),
			kg[0] + kg[1] + kg[2] + kg[3], kg[BalanceBoardMath::kTopLeft], kg[BalanceBoardMath::kTopRight],
			kg[BalanceBoardMath::kBottomLeft], kg[BalanceBoardMath::kBottomRight]);
	}

	// the options only drive the emulated board
	if (m_weight->IsEnabled() == real.assigned)
	{
		for (auto* slider : {m_weight, m_foot_separation, m_foot_length, m_battery})
			slider->Enable(!real.assigned);
	}
	if (m_status->GetLabel() != label)
	{
		const bool relayout = m_status->GetLabel().Freq('\n') != label.Freq('\n');
		m_status->SetLabel(label);
		if (relayout)
			Layout();
	}
}

void BalanceBoardInputPanel::load_controller(const EmulatedControllerPtr& emulated_controller)
{
	InputPanel::load_controller(emulated_controller);

	const auto board = std::dynamic_pointer_cast<BalanceBoardController>(emulated_controller);
	m_board = board;
	if (!board)
		return;

	m_weight->SetValue((int)std::lround(board->get_weight()));
	m_foot_separation->SetValue((int)std::lround(board->get_foot_separation()));
	m_foot_length->SetValue((int)std::lround(board->get_foot_length()));
	m_battery->SetValue(board->get_battery());
	update_setting_texts();
}
