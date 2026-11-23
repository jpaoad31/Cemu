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
#include "wxgui/input/LiveIndicators.h"

#ifdef HAS_IOBLUETOOTH
#include "input/api/Wiimote/darwin/IOBluetoothWiimote.h"
#endif
#ifdef SUPPORTS_WIIMOTE
#include "wxgui/input/panels/WiiBridgePanel.h"
#include "config/CemuConfig.h"
#endif

BalanceBoardInputPanel::BalanceBoardInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	auto bold_font = GetFont();
	bold_font.MakeBold();

	auto* main_sizer = new wxBoxSizer(wxVERTICAL);

	auto* info = new wxStaticText(this, wxID_ANY, _("The Balance Board always uses Wii Remote channel 4 (WPAD channel 3). Use a Wiimote on another controller for the menus.\nReal board: connect it (through a Wii BT Bridge, below on macOS, or pair it in the Bluetooth settings), then choose it as the device above (Refresh, or Other device... > Wiimote). While the device is a connected Balance Board it is the source and the options below are unused; with Keyboard/Mouse or a pad the board is emulated; a Wii Remote or a board that isn't connected falls back to the emulated board and the line at the bottom says why."));
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
	// the Wii BT Bridge settings (shared with the Wii Remote panel)
	main_sizer->Add(new WiiBridgePanel(this), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);
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
	text = new wxStaticText(this, wxID_ANY, _("Centre of balance"));
	text->SetToolTip(_("Where the weight is (like a stick). Default with the Keyboard/Mouse device: the cursor over the TV window (up is the front). With a real board as the device its sensors are used instead."));
	text->SetFont(bold_font);
	m_item_sizer->Add(text, wxGBPosition(row, column), wxGBSpan(1, 2), wxALL | wxEXPAND, 5);
	for (const auto id : {BalanceBoardController::kButtonId_Balance_Up, BalanceBoardController::kButtonId_Balance_Down,
		BalanceBoardController::kButtonId_Balance_Left, BalanceBoardController::kButtonId_Balance_Right})
	{
		add_button_row(++row, column, id);
	}
	m_draw = new wxPointerBox(this, 1.0f, true);
	m_item_sizer->Add(m_draw, wxGBPosition(++row, column + 1), wxGBSpan(2, 1), wxTOP | wxBOTTOM | wxALIGN_CENTER, 5);
	m_item_sizer->Add(create_clear_button(this, {BalanceBoardController::kButtonId_Balance_Up, BalanceBoardController::kButtonId_Balance_Down,
		BalanceBoardController::kButtonId_Balance_Left, BalanceBoardController::kButtonId_Balance_Right}, _("the centre of balance")),
		wxGBPosition(row, column), wxDefaultSpan, wxALL | wxALIGN_TOP, 5);

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

static bool board_is_axis(BalanceBoardController::ButtonId id)
{
	return id >= BalanceBoardController::kButtonId_Balance_Up && id <= BalanceBoardController::kButtonId_Balance_Right;
}

void BalanceBoardInputPanel::add_button_row(sint32 row, sint32 column, BalanceBoardController::ButtonId button_id)
{
	m_item_sizer->Add(
		new wxStaticText(this, wxID_ANY, wxGetTranslation(wxString::FromUTF8(BalanceBoardController::get_button_name(button_id)))),
		wxGBPosition(row, column),
		wxDefaultSpan,
		wxALL | wxALIGN_CENTER_VERTICAL, 5);

	auto* text_ctrl = create_mapping_control(this, button_id, wxGetTranslation(wxString::FromUTF8(BalanceBoardController::get_button_name(button_id))), board_is_axis(button_id));
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
}

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
	// the data source in one line (real board, emulated, or emulated because the assigned real
	// device isn't a connected board), then the live values of that source
	const auto source = board->get_source_info();
	const bool real_source = source.source == BalanceBoardController::Source::Real;
	m_draw->SetCaption(wxString());
	if (real_source)
	{
		// a real board is the source: its own load cells, converted with its own calibration
		label = wxString::FromUTF8(source.text);
		if (real.ready() && !real.crc_ok)
			label += _(" (the calibration CRC does NOT match)");

		if (real.calibrated && real.has_data)
		{
			const auto cal = BalanceBoardMath::ParseCalibration(real.data.memory);
			double kg[BalanceBoardMath::kSensorCount];
			std::array<double, BalanceBoardMath::kSensorCount> sensors;
			for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
				sensors[i] = kg[i] = BalanceBoardMath::RawToKg(cal, i, real.data.raw[i], cal.raw[i][0]);
			double x, y;
			BalanceBoardMath::SensorsToCenterOfBalance(sensors, x, y);
			show_balance(true, (float)x, (float)y);
			label += wxString::Format(_("\nTotal %.1f kg (TL %.1f  TR %.1f  BL %.1f  BR %.1f)  Temperature %d  Battery 0x%02x (level %d/4)%s"),
				kg[0] + kg[1] + kg[2] + kg[3], kg[BalanceBoardMath::kTopLeft], kg[BalanceBoardMath::kTopRight],
				kg[BalanceBoardMath::kBottomLeft], kg[BalanceBoardMath::kBottomRight], (int)(sint8)real.data.temperature,
				real.data.battery, BalanceBoardMath::BatteryRawToLevel(real.data.battery), (board->is_a_down() ? _("  Power pressed") : wxString()));
		}
		else
			show_balance(false, 0, 0);
	}
	else
	{
		const auto axis = board->get_axis();
		show_balance(true, axis.x, axis.y);
		m_draw->SetCaption(board->is_balance_mapped() ? wxString() : _("not bound: centred"));

		const auto raw = board->get_sensor_values();
		double kg[BalanceBoardMath::kSensorCount];
		for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
			kg[i] = BalanceBoardMath::RawToKg(raw[i]);
		label = wxString::FromUTF8(source.text) + "\n" + wxString::Format(_("%s  Total %.1f kg (TL %.1f  TR %.1f  BL %.1f  BR %.1f)"),
			board->is_stepped_off() ? _("Stepped off.") : _("On the board."),
			kg[0] + kg[1] + kg[2] + kg[3], kg[BalanceBoardMath::kTopLeft], kg[BalanceBoardMath::kTopRight],
			kg[BalanceBoardMath::kBottomLeft], kg[BalanceBoardMath::kBottomRight]);
	}

	// the options only drive the emulated board
	if (m_weight->IsEnabled() == real_source)
	{
		for (auto* slider : {m_weight, m_foot_separation, m_foot_length, m_battery})
			slider->Enable(!real_source);
	}
	const wxColour colour = source.warning ? wxTheColourDatabase->Find("ERROR") : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	if (m_status->GetForegroundColour() != colour)
		m_status->SetForegroundColour(colour);
	if (m_status->GetLabel() != label)
	{
		const bool relayout = m_status->GetLabel().Freq('\n') != label.Freq('\n');
		m_status->SetLabel(label);
		if (relayout)
			Layout();
	}
}

void BalanceBoardInputPanel::show_balance(bool has_point, float x, float y)
{
	wxPointerBox::State state;
	state.has_point = has_point;
	state.pressed = has_point;
	state.x = (std::clamp(x, -1.0f, 1.0f) + 1.0f) * 0.5f;
	state.y = (1.0f - std::clamp(y, -1.0f, 1.0f)) * 0.5f;
	m_draw->SetState(state);
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
