#include "wxgui/input/panels/VPADInputPanel.h"

#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/button.h>

#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/components/wxInputDraw.h"
#include "wxgui/input/LiveIndicators.h"
#include "input/emulated/VPADController.h"

namespace
{
	wxString name(VPADController::ButtonId id)
	{
		return wxGetTranslation(wxString::FromUTF8(VPADController::get_button_name(id)));
	}
}

// grouped like Dolphin's mapping pages: face buttons and shoulders, sticks with their indicators,
// D-pad, GamePad extras, motion and touch
VPADInputPanel::VPADInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	using V = VPADController;
	auto* main_sizer = new wxBoxSizer(wxHORIZONTAL);
	const int gap = FromDIP(4);

	// buttons
	auto* col1 = new wxBoxSizer(wxVERTICAL);
	col1->Add(create_group(this, _("Buttons"), {
		{V::kButtonId_A, "A"}, {V::kButtonId_B, "B"}, {V::kButtonId_X, "X"}, {V::kButtonId_Y, "Y"},
		{V::kButtonId_L, "L"}, {V::kButtonId_R, "R"}, {V::kButtonId_ZL, "ZL"}, {V::kButtonId_ZR, "ZR"},
		{V::kButtonId_Plus, "+"}, {V::kButtonId_Minus, "-"}, {V::kButtonId_Home, _("Home")},
	}), 0, wxEXPAND | wxALL, gap);
	main_sizer->Add(col1, 1, wxEXPAND);

	// left stick + D-pad
	auto* col2 = new wxBoxSizer(wxVERTICAL);
	auto* left = create_group(this, _("Left stick"), {
		{V::kButtonId_StickL_Up, name(V::kButtonId_StickL_Up)}, {V::kButtonId_StickL_Down, name(V::kButtonId_StickL_Down)},
		{V::kButtonId_StickL_Left, name(V::kButtonId_StickL_Left)}, {V::kButtonId_StickL_Right, name(V::kButtonId_StickL_Right)},
		{V::kButtonId_StickL, name(V::kButtonId_StickL)},
	}, true);
	m_left_draw = new wxInputDraw(left->GetStaticBox(), wxID_ANY, wxDefaultPosition, FromDIP(wxSize(64, 64)));
	left->Add(m_left_draw, 0, wxALIGN_CENTER | wxALL, gap);
	col2->Add(left, 0, wxEXPAND | wxALL, gap);
	col2->Add(create_group(this, _("D-pad"), {
		{V::kButtonId_Up, name(V::kButtonId_Up)}, {V::kButtonId_Down, name(V::kButtonId_Down)},
		{V::kButtonId_Left, name(V::kButtonId_Left)}, {V::kButtonId_Right, name(V::kButtonId_Right)},
	}), 0, wxEXPAND | wxALL, gap);
	main_sizer->Add(col2, 1, wxEXPAND);

	// right stick + extras
	auto* col3 = new wxBoxSizer(wxVERTICAL);
	auto* right = create_group(this, _("Right stick"), {
		{V::kButtonId_StickR_Up, name(V::kButtonId_StickR_Up)}, {V::kButtonId_StickR_Down, name(V::kButtonId_StickR_Down)},
		{V::kButtonId_StickR_Left, name(V::kButtonId_StickR_Left)}, {V::kButtonId_StickR_Right, name(V::kButtonId_StickR_Right)},
		{V::kButtonId_StickR, name(V::kButtonId_StickR)},
	}, true);
	m_right_draw = new wxInputDraw(right->GetStaticBox(), wxID_ANY, wxDefaultPosition, FromDIP(wxSize(64, 64)));
	right->Add(m_right_draw, 0, wxALIGN_CENTER | wxALL, gap);
	col3->Add(right, 0, wxEXPAND | wxALL, gap);
	auto* extras = create_group(this, _("GamePad"), {
		{V::kButtonId_Mic, _("blow mic")}, {V::kButtonId_Screen, _("show screen")},
	});
	m_togglePadViewCheckBox = new wxCheckBox(extras->GetStaticBox(), wxID_ANY, _("\"show screen\" toggles"));
	m_togglePadViewCheckBox->SetToolTip(_("Makes the \"show screen\" button toggle between the TV and gamepad screens"));
	extras->Add(m_togglePadViewCheckBox, 0, wxALL, gap);
	col3->Add(extras, 0, wxEXPAND | wxALL, gap);
	main_sizer->Add(col3, 1, wxEXPAND);

	// touch and motion: explicit groups (fork): nothing uses the mouse unless a binding says so
	auto* outer = new wxBoxSizer(wxVERTICAL);
	outer->Add(main_sizer, 0, wxEXPAND);
	auto* row2 = new wxBoxSizer(wxHORIZONTAL);

	wxFlexGridSizer* grid = nullptr;
	auto* touch = create_group(this, _("Touch screen"), {
		{V::kButtonId_Touch_Up, name(V::kButtonId_Touch_Up)}, {V::kButtonId_Touch_Down, name(V::kButtonId_Touch_Down)},
		{V::kButtonId_Touch_Left, name(V::kButtonId_Touch_Left)}, {V::kButtonId_Touch_Right, name(V::kButtonId_Touch_Right)},
		{V::kButtonId_Touch_Press, name(V::kButtonId_Touch_Press)},
	}, true, &grid);
	touch->GetStaticBox()->SetToolTip(_("Where the GamePad screen is touched (like a stick: the cursor's half axes) and when. Default with the Keyboard/Mouse device: the cursor over the GamePad window (the main window when there is none) and its left button. With the Remote GamePad as the device, the phone's touch screen is the touch screen (the mouse then needs explicit Keyboard/Mouse inputs here, in Advanced...)."));
	m_touch = new wxPointerBox(touch->GetStaticBox(), 16.0f / 9.0f);
	touch->Add(m_touch, 0, wxEXPAND | wxALL, gap);
	touch->Add(create_clear_button(touch->GetStaticBox(), {V::kButtonId_Touch_Up, V::kButtonId_Touch_Down, V::kButtonId_Touch_Left, V::kButtonId_Touch_Right, V::kButtonId_Touch_Press}, _("the touch screen")), 0, wxALL, gap);
	row2->Add(touch, 1, wxEXPAND | wxALL, gap);

	auto* motion = new wxStaticBoxSizer(wxVERTICAL, this, _("Motion"));
	m_motion_source = new wxStaticText(motion->GetStaticBox(), wxID_ANY, wxEmptyString);
	motion->Add(m_motion_source, 0, wxEXPAND | wxALL, gap);
	m_motion = new wxValueBars(motion->GetStaticBox(), {_("Accel X"), _("Accel Y"), _("Accel Z"), _("Gyro X"), _("Gyro Y"), _("Gyro Z")});
	m_motion->SetToolTip(_("From a device with \"Use motion\" on (SDL, DSU, Remote GamePad; in the device's Settings). It wins over the emulated tilt."));
	motion->Add(m_motion, 0, wxEXPAND | wxALL, gap);
	auto* tilt = create_group(motion->GetStaticBox(), _("Emulated tilt (when the device gives no motion)"), {
		{V::kButtonId_Tilt_Up, name(V::kButtonId_Tilt_Up)}, {V::kButtonId_Tilt_Down, name(V::kButtonId_Tilt_Down)},
		{V::kButtonId_Tilt_Left, name(V::kButtonId_Tilt_Left)}, {V::kButtonId_Tilt_Right, name(V::kButtonId_Tilt_Right)},
		{V::kButtonId_Turn_Left, name(V::kButtonId_Turn_Left)}, {V::kButtonId_Turn_Right, name(V::kButtonId_Turn_Right)},
		{V::kButtonId_Tilt_Hold, name(V::kButtonId_Tilt_Hold)},
	}, true);
	tilt->GetStaticBox()->SetToolTip(_("Used when the device gives no motion. Tilts the GamePad by a position (the Keyboard/Mouse default \"mouse drag\": the cursor while the right button is held) and turns it (the wheel). With \"hold to tilt\" bound, it only tilts while that is held."));
	m_tilt = new wxValueBars(tilt->GetStaticBox(), {_("Tilt X"), _("Tilt Y"), _("Turn")});
	tilt->Add(m_tilt, 0, wxEXPAND | wxALL, gap);
	tilt->Add(create_clear_button(tilt->GetStaticBox(), {V::kButtonId_Tilt_Up, V::kButtonId_Tilt_Down, V::kButtonId_Tilt_Left, V::kButtonId_Tilt_Right, V::kButtonId_Turn_Left, V::kButtonId_Turn_Right, V::kButtonId_Tilt_Hold}, _("the emulated tilt")), 0, wxALL, gap);
	motion->Add(tilt, 0, wxEXPAND | wxALL, gap);
	row2->Add(motion, 1, wxEXPAND | wxALL, gap);

	outer->Add(row2, 0, wxEXPAND);
	SetSizer(outer);
	Layout();
}

void VPADInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller_base)
{
	InputPanel::on_timer(emulated_controller, controller_base);
	if (!emulated_controller)
		return;

	static_cast<VPADController*>(emulated_controller.get())->set_screen_toggle(m_togglePadViewCheckBox->GetValue());

	m_left_draw->SetAxisValue(emulated_controller->get_axis());
	m_right_draw->SetAxisValue(emulated_controller->get_rotation());

	auto* vpad = static_cast<VPADController*>(emulated_controller.get());
	const auto tilt = vpad->get_tilt();
	const auto set_label = [](wxStaticText* t, const wxString& text) {
		if (t->GetLabel() != text)
			t->SetLabel(text);
	};
	if (emulated_controller->has_motion())
	{
		auto sample = emulated_controller->get_motion_data();
		float acc[3], gyro[3];
		sample.getAccelerometer(acc);
		sample.getGyrometer(gyro);
		m_motion->SetValues({acc[0] / 2, acc[1] / 2, acc[2] / 2, gyro[0] / 8, gyro[1] / 8, gyro[2] / 8});
		set_label(m_motion_source, _("Source: the device's motion (\"Use motion\")."));
	}
	else
	{
		m_motion->SetInactive(_("The device gives no motion (\"Use motion\" off or none)"));
		set_label(m_motion_source, tilt.bound ? _("Source: the emulated tilt below.") : _("No motion: the device has none, no tilt bindings."));
	}
	if (tilt.bound)
		m_tilt->SetValues({tilt.active ? tilt.tilt.x : 0.0f, tilt.active ? tilt.tilt.y : 0.0f, tilt.active ? tilt.turn : 0.0f});
	else
		m_tilt->SetInactive(_("Not bound"));

	const auto touch = vpad->get_touch();
	wxPointerBox::State state;
	state.has_point = touch.bound;
	state.pressed = touch.touching;
	state.x = touch.position.x;
	state.y = touch.position.y;
	m_touch->SetState(state);
	const auto device = emulated_controller->get_device();
	const bool remote = device && device->api() == InputAPI::RemoteGamePad;
	wxString caption = touch.device ? _("phone touching") : touch.touching ? _("touching") : touch.bound ? wxString() : _("not bound");
	if (remote && !touch.device)
		caption += (caption.empty() ? "" : " - ") + _("the phone's touch screen");
	m_touch->SetCaption(caption);
}

void VPADInputPanel::load_controller(const EmulatedControllerPtr& controller)
{
	InputPanel::load_controller(controller);
	if (controller)
		m_togglePadViewCheckBox->SetValue(static_cast<VPADController*>(controller.get())->is_screen_active_toggle());
}
