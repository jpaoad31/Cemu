#include "wxgui/input/panels/WiimoteInputPanel.h"

#include <wx/button.h>
#include <wx/gbsizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/slider.h>
#include <wx/checkbox.h>

#include "wxgui/helpers/wxControlObject.h"
#include "input/emulated/WiimoteController.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/components/wxInputDraw.h"
#include "wxgui/input/LiveIndicators.h"
#include <wx/statbox.h>
#include <wx/sizer.h>
#ifdef SUPPORTS_WIIMOTE
#include "wxgui/input/panels/WiiBridgePanel.h"
#endif

constexpr WiimoteController::ButtonId g_kFirstColumnItems[] =
{
	WiimoteController::kButtonId_A, WiimoteController::kButtonId_B, WiimoteController::kButtonId_1, WiimoteController::kButtonId_2, WiimoteController::kButtonId_Plus, WiimoteController::kButtonId_Minus, WiimoteController::kButtonId_Home
};

constexpr WiimoteController::ButtonId g_kSecondColumnItems[] =
{
	WiimoteController::kButtonId_Up, WiimoteController::kButtonId_Down, WiimoteController::kButtonId_Left, WiimoteController::kButtonId_Right
};

constexpr WiimoteController::ButtonId g_kThirdColumnItems[] =
{
	WiimoteController::kButtonId_Nunchuck_C, WiimoteController::kButtonId_Nunchuck_Z,
	WiimoteController::kButtonId_None,
	WiimoteController::kButtonId_Nunchuck_Up,WiimoteController::kButtonId_Nunchuck_Down,WiimoteController::kButtonId_Nunchuck_Left,WiimoteController::kButtonId_Nunchuck_Right
};

WiimoteInputPanel::WiimoteInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	auto bold_font = GetFont();
	bold_font.MakeBold();

	auto* main_sizer = new wxBoxSizer(wxVERTICAL);
    auto* horiz_main_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto* extensions_sizer = new wxBoxSizer(wxHORIZONTAL);
    horiz_main_sizer->Add(extensions_sizer, wxSizerFlags(0).Align(wxALIGN_CENTER_VERTICAL));

    extensions_sizer->Add(new wxStaticText(this, wxID_ANY, _("Extensions:")));
    extensions_sizer->AddSpacer(10);

	m_motion_plus = new wxCheckBox(this, wxID_ANY, _("MotionPlus"));
	m_motion_plus->Bind(wxEVT_CHECKBOX, &WiimoteInputPanel::on_extension_change, this);
	extensions_sizer->Add(m_motion_plus);

	m_nunchuck = new wxCheckBox(this, wxID_ANY, _("Nunchuck"));
	m_nunchuck->Bind(wxEVT_CHECKBOX, &WiimoteInputPanel::on_extension_change, this);
	extensions_sizer->Add(m_nunchuck);

	m_classic = new wxCheckBox(this, wxID_ANY, _("Classic"));
	m_classic->Bind(wxEVT_CHECKBOX, &WiimoteInputPanel::on_extension_change, this);
	m_classic->Hide();
	extensions_sizer->Add(m_classic);

	main_sizer->Add(horiz_main_sizer, 0, wxEXPAND | wxALL, 5);
#ifdef SUPPORTS_WIIMOTE
	// the Wii BT Bridge settings (shared with the Balance Board panel): real Wii Remotes on systems
	// that can't pair them (macOS)
	main_sizer->Add(new WiiBridgePanel(this), 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);
#endif
	main_sizer->Add(new wxStaticLine(this), 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 5);

	m_item_sizer = new wxGridBagSizer();

	sint32 row = 0;
	sint32 column = 0;
	for (const auto& id : g_kFirstColumnItems)
	{
		row++;
		add_button_row(row, column, id);
	}

	m_item_sizer->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVERTICAL), wxGBPosition(0, column + 2), wxGBSpan(11, 1), wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

	//////////////////////////////////////////////////////////////////

	row = 0;
	column += 3;

	auto text = new wxStaticText(this, wxID_ANY, _("D-pad"));
	text->SetFont(bold_font);
	m_item_sizer->Add(text, wxGBPosition(row, column), wxGBSpan(1, 3), wxALL | wxEXPAND, 5);

	for (const auto& id : g_kSecondColumnItems)
	{
		row++;
		add_button_row(row, column, id);
	}

	row = 8;
	// Volume
	text = new wxStaticText(this, wxID_ANY, _("Volume"));
	text->Disable();
	m_item_sizer->Add(text, wxGBPosition(row, column), wxDefaultSpan, wxALL, 5);

	m_volume = new wxSlider(this, wxID_ANY, 0, 0, 100);
	m_volume->Disable();
	m_item_sizer->Add(m_volume, wxGBPosition(row, column + 1), wxDefaultSpan, wxTOP | wxBOTTOM | wxEXPAND, 5);

	const auto volume_text = new wxStaticText(this, wxID_ANY, wxString::Format("%d%%", 0));
	volume_text->Disable();
	m_item_sizer->Add(volume_text, wxGBPosition(row, column + 2), wxDefaultSpan, wxALL, 5);
	m_volume->Bind(wxEVT_SLIDER, &WiimoteInputPanel::on_volume_change, this, wxID_ANY, wxID_ANY, new wxControlObject(volume_text));
	row++;

	m_item_sizer->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVERTICAL), wxGBPosition(0, column + 3), wxGBSpan(11, 1), wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

	//////////////////////////////////////////////////////////////////

	row = 0;
	column += 4;

	text = new wxStaticText(this, wxID_ANY, _("Nunchuck"));
	text->SetFont(bold_font);
	m_item_sizer->Add(text, wxGBPosition(row, column), wxGBSpan(1, 3), wxALL | wxEXPAND, 5);

	for (const auto& id : g_kThirdColumnItems)
	{
		row++;
		if (id == WiimoteController::kButtonId_None)
			continue;

		m_item_sizer->Add(
			new wxStaticText(this, wxID_ANY, wxGetTranslation(wxString::FromUTF8(WiimoteController::get_button_name(id)))),
			wxGBPosition(row, column),
			wxDefaultSpan,
			wxALL | wxALIGN_CENTER_VERTICAL, 5);

		auto* text_ctrl = create_mapping_control(this, id, wxGetTranslation(wxString::FromUTF8(WiimoteController::get_button_name(id))), id >= WiimoteController::kButtonId_Nunchuck_Up);
		text_ctrl->Enable(m_nunchuck->GetValue());
		m_item_sizer->Add(text_ctrl, wxGBPosition(row, column + 1), wxDefaultSpan, wxALL | wxEXPAND, 5);

		m_nunchuck_items.push_back(text_ctrl);
	}


	// input drawer
	m_draw = new wxInputDraw(this, wxID_ANY, wxDefaultPosition, { 60, 60 });
	m_draw->Enable(m_nunchuck->GetValue());
	m_item_sizer->Add(5, 0, wxGBPosition(3, column + 3), wxDefaultSpan, wxTOP | wxBOTTOM | wxEXPAND | wxALIGN_CENTER, 5);
	m_item_sizer->Add(m_draw, wxGBPosition(3, column + 4), wxGBSpan(4, 1), wxTOP | wxBOTTOM | wxEXPAND | wxALIGN_CENTER, 5);

	m_nunchuck_items.push_back(m_draw);

	//////////////////////////////////////////////////////////////////

	main_sizer->Add(m_item_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);

	// fork: the pointer (IR camera) as bindings, after Dolphin's Point group. A real Wii Remote's own
	// IR wins while it sees the sensor bar.
	{
		using W = WiimoteController;
		const auto label = [](W::ButtonId id) { return wxGetTranslation(wxString::FromUTF8(W::get_button_name(id))); };
		auto* pointer_row = new wxBoxSizer(wxHORIZONTAL);
		auto* group = create_group(this, _("Pointer (IR)"), {
			{W::kButtonId_Pointer_Up, label(W::kButtonId_Pointer_Up)}, {W::kButtonId_Pointer_Down, label(W::kButtonId_Pointer_Down)},
			{W::kButtonId_Pointer_Left, label(W::kButtonId_Pointer_Left)}, {W::kButtonId_Pointer_Right, label(W::kButtonId_Pointer_Right)},
			{W::kButtonId_Pointer_Hide, label(W::kButtonId_Pointer_Hide)}, {W::kButtonId_Pointer_Recenter, label(W::kButtonId_Pointer_Recenter)},
		}, true);
		group->GetStaticBox()->SetToolTip(_("Where the Wii Remote points on the TV. Default with the Keyboard/Mouse device: the cursor over the TV window. With a real Wii Remote as the device, its IR camera points while it sees the sensor bar."));
		group->Add(create_clear_button(group->GetStaticBox(), {W::kButtonId_Pointer_Up, W::kButtonId_Pointer_Down, W::kButtonId_Pointer_Left, W::kButtonId_Pointer_Right, W::kButtonId_Pointer_Hide, W::kButtonId_Pointer_Recenter}, _("the pointer")), 0, wxALL, 4);
		pointer_row->Add(group, 1, wxEXPAND | wxALL, 4);

		auto* settings = new wxStaticBoxSizer(wxVERTICAL, this, _("Pointer settings"));
		auto* parent = settings->GetStaticBox();
		auto* grid = new wxFlexGridSizer(2, 4, 6);
		grid->AddGrowableCol(1, 1);
		const auto slider = [&](const wxString& text, int min, int max, int value, const wxString& tip) {
			grid->Add(new wxStaticText(parent, wxID_ANY, text), 0, wxALIGN_CENTER_VERTICAL);
			auto* s = new wxSlider(parent, wxID_ANY, value, min, max, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL | wxSL_VALUE_LABEL);
			s->SetMinSize(wxSize(FromDIP(140), -1));
			s->SetToolTip(tip);
			s->Bind(wxEVT_SLIDER, &WiimoteInputPanel::on_pointer_setting, this);
			grid->Add(s, 1, wxEXPAND);
			return s;
		};
		m_pointer_center_x = slider(_("Center X %"), -100, 100, 0, _("Moves the pointer's centre (percent of half the screen)."));
		m_pointer_center_y = slider(_("Center Y %"), -100, 100, 0, _("Moves the pointer's centre (percent of half the screen)."));
		m_pointer_width = slider(_("Width %"), 5, 400, 100, _("100: the binding's full range covers the screen width."));
		m_pointer_height = slider(_("Height %"), 5, 400, 100, _("100: the binding's full range covers the screen height."));
		settings->Add(grid, 0, wxEXPAND | wxALL, 4);
		m_pointer_relative = new wxCheckBox(parent, wxID_ANY, _("Relative input"));
		m_pointer_relative->SetToolTip(_("The binding moves the pointer (a stick: a full deflection crosses the screen in a second) instead of placing it. \"recenter\" puts it back in the middle."));
		m_pointer_relative->Bind(wxEVT_CHECKBOX, &WiimoteInputPanel::on_pointer_setting, this);
		settings->Add(m_pointer_relative, 0, wxALL, 4);
		m_pointer_auto_hide = new wxCheckBox(parent, wxID_ANY, _("Auto-hide (2 s without movement)"));
		m_pointer_auto_hide->Bind(wxEVT_CHECKBOX, &WiimoteInputPanel::on_pointer_setting, this);
		settings->Add(m_pointer_auto_hide, 0, wxALL, 4);
		m_pointer = new wxPointerBox(parent, 16.0f / 9.0f, true);
		settings->Add(m_pointer, 0, wxEXPAND | wxALL, 4);
		pointer_row->Add(settings, 1, wxEXPAND | wxALL, 4);
		main_sizer->Add(pointer_row, 0, wxEXPAND | wxLEFT | wxRIGHT, 5);
	}

	SetSizer(main_sizer);
	Layout();
}

void WiimoteInputPanel::add_button_row(sint32 row, sint32 column, const WiimoteController::ButtonId &button_id) {
	m_item_sizer->Add(
		new wxStaticText(this, wxID_ANY, wxGetTranslation(wxString::FromUTF8(WiimoteController::get_button_name(button_id)))),
		wxGBPosition(row, column),
		wxDefaultSpan,
		wxALL | wxALIGN_CENTER_VERTICAL, 5);

	auto* text_ctrl = create_mapping_control(this, button_id, wxGetTranslation(wxString::FromUTF8(WiimoteController::get_button_name(button_id))));
	m_item_sizer->Add(text_ctrl, wxGBPosition(row, column + 1), wxDefaultSpan, wxALL | wxEXPAND, 5);
}

void WiimoteInputPanel::set_active_device_type(WPADDeviceType type)
{
	m_device_type = type;

	m_motion_plus->SetValue(type == kWAPDevMPLS || type == kWAPDevMPLSFreeStyle || type == kWAPDevMPLSClassic);
	switch(type)
	{
	case kWAPDevFreestyle: 
	case kWAPDevMPLSFreeStyle:
		m_nunchuck->SetValue(true);
		m_classic->SetValue(false);
		for (const auto& item : m_nunchuck_items)
		{
			item->Enable(true);
		}
		break;

	case kWAPDevClassic: 
	case kWAPDevMPLSClassic:
		m_nunchuck->SetValue(false);
		m_classic->SetValue(true);
		for (const auto& item : m_nunchuck_items)
		{
			item->Enable(false);
		}
		break;

	default:
		m_nunchuck->SetValue(false);
		m_classic->SetValue(false);
		for (const auto& item : m_nunchuck_items)
		{
			item->Enable(false);
		}
	}
}

void WiimoteInputPanel::on_pointer_setting(wxCommandEvent& event)
{
	const auto wiimote = std::dynamic_pointer_cast<WiimoteController>(m_controller);
	if (!wiimote)
		return;
	WiimoteController::PointerSettings settings;
	settings.center_x = m_pointer_center_x->GetValue() / 100.0f;
	settings.center_y = m_pointer_center_y->GetValue() / 100.0f;
	settings.width = m_pointer_width->GetValue() / 100.0f;
	settings.height = m_pointer_height->GetValue() / 100.0f;
	settings.relative = m_pointer_relative->GetValue();
	settings.auto_hide = m_pointer_auto_hide->GetValue();
	wiimote->set_pointer_settings(settings);
}

void WiimoteInputPanel::show_pointer_settings(const WiimoteController::PointerSettings& settings)
{
	m_pointer_center_x->SetValue((int)std::lround(settings.center_x * 100));
	m_pointer_center_y->SetValue((int)std::lround(settings.center_y * 100));
	m_pointer_width->SetValue((int)std::lround(settings.width * 100));
	m_pointer_height->SetValue((int)std::lround(settings.height * 100));
	m_pointer_relative->SetValue(settings.relative);
	m_pointer_auto_hide->SetValue(settings.auto_hide);
}

void WiimoteInputPanel::on_volume_change(wxCommandEvent& event)
{
}

void WiimoteInputPanel::on_extension_change(wxCommandEvent& event)
{
	if(m_motion_plus->GetValue() && m_nunchuck->GetValue())
		set_active_device_type(kWAPDevMPLSFreeStyle);
	else if(m_motion_plus->GetValue() && m_classic->GetValue())
		set_active_device_type(kWAPDevMPLSClassic);
	else if (m_motion_plus->GetValue())
		set_active_device_type(kWAPDevMPLS);
	else if (m_nunchuck->GetValue())
		set_active_device_type(kWAPDevFreestyle);
	else if (m_classic->GetValue())
		set_active_device_type(kWAPDevClassic);
	else 
		set_active_device_type(kWAPDevCore);
}

void WiimoteInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	if (emulated_controller)
	{
		const auto wiimote = std::dynamic_pointer_cast<WiimoteController>(emulated_controller);
		wxASSERT(wiimote);

		wiimote->set_device_type(m_device_type);
	}

	InputPanel::on_timer(emulated_controller, controller);

	if (emulated_controller)
	{
		const auto axis = emulated_controller->get_axis();
		m_draw->SetAxisValue(axis);

		// the pointer the game gets (a real Wii Remote's IR, else the bindings)
		const auto wiimote = std::static_pointer_cast<WiimoteController>(emulated_controller);
		const auto sample = wiimote->get_kpad_pointer();
		const auto bindings = wiimote->get_pointer();
		wxPointerBox::State state;
		if (sample.visibility != PositionVisibility::NONE)
		{
			state.has_point = state.pressed = true;
			state.x = sample.position.x;
			state.y = sample.position.y;
		}
		else if (bindings.bound)
		{
			state.has_point = true; // hidden: hollow
			state.x = bindings.position.x;
			state.y = bindings.position.y;
		}
		m_pointer->SetState(state);
		m_pointer->SetCaption(!bindings.bound && sample.visibility == PositionVisibility::NONE ? _("not bound")
			: sample.visibility == PositionVisibility::NONE ? _("hidden") : wxString());
	}
}

void WiimoteInputPanel::load_controller(const EmulatedControllerPtr& emulated_controller)
{
	InputPanel::load_controller(emulated_controller);

	if (emulated_controller) {
		const auto wiimote = std::dynamic_pointer_cast<WiimoteController>(emulated_controller);
		wxASSERT(wiimote);
		set_active_device_type(wiimote->get_device_type());
		show_pointer_settings(wiimote->get_pointer_settings());
	}
}
