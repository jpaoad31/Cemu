#include "wxgui/input/panels/BalanceBoardInputPanel.h"

#include <wx/gbsizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/slider.h>

#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/components/wxInputDraw.h"

BalanceBoardInputPanel::BalanceBoardInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	auto bold_font = GetFont();
	bold_font.MakeBold();

	auto* main_sizer = new wxBoxSizer(wxVERTICAL);

	auto* info = new wxStaticText(this, wxID_ANY, _("The Balance Board always uses Wii Remote channel 4 (WPAD channel 3). Use a Wiimote on another controller for the menus."));
	info->Wrap(600);
	main_sizer->Add(info, 0, wxALL, 5);
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

void BalanceBoardInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	InputPanel::on_timer(emulated_controller, controller);

	const auto board = std::dynamic_pointer_cast<BalanceBoardController>(emulated_controller);
	if (!board)
		return;

	m_draw->SetAxisValue(board->get_axis());

	const auto raw = board->get_sensor_values();
	double kg[BalanceBoardMath::kSensorCount];
	for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
		kg[i] = BalanceBoardMath::RawToKg(raw[i]);
	const auto label = wxString::Format(_("%s  Total %.1f kg (TL %.1f  TR %.1f  BL %.1f  BR %.1f)"),
		board->is_stepped_off() ? _("Stepped off.") : _("On the board."),
		kg[0] + kg[1] + kg[2] + kg[3], kg[BalanceBoardMath::kTopLeft], kg[BalanceBoardMath::kTopRight],
		kg[BalanceBoardMath::kBottomLeft], kg[BalanceBoardMath::kBottomRight]);
	if (m_status->GetLabel() != label)
		m_status->SetLabel(label);
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
