#include "wxgui/input/panels/VPADInputPanel.h"

#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/checkbox.h>

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

	// motion and touch (live)
	auto* col4 = new wxBoxSizer(wxVERTICAL);
	auto* motion = new wxStaticBoxSizer(wxVERTICAL, this, _("Motion"));
	m_motion = new wxValueBars(motion->GetStaticBox(), {_("Accel X"), _("Accel Y"), _("Accel Z"), _("Gyro X"), _("Gyro Y"), _("Gyro Z")});
	m_motion->SetToolTip(_("From the devices with \"Use motion\" on (SDL, DSU, Remote GamePad; in the device's Settings). Without one, drag with the right mouse button in the game window."));
	motion->Add(m_motion, 0, wxEXPAND | wxALL, gap);
	col4->Add(motion, 0, wxEXPAND | wxALL, gap);
	auto* touch = new wxStaticBoxSizer(wxVERTICAL, this, _("Touch screen"));
	m_touch = new wxTouchDraw(touch->GetStaticBox());
	m_touch->SetToolTip(_("Touch from a device (Remote GamePad), else the left mouse button on the GamePad image."));
	touch->Add(m_touch, 0, wxEXPAND | wxALL, gap);
	col4->Add(touch, 0, wxEXPAND | wxALL, gap);
	main_sizer->Add(col4, 1, wxEXPAND);

	SetSizer(main_sizer);
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

	if (emulated_controller->has_motion())
	{
		auto sample = emulated_controller->get_motion_data();
		float acc[3], gyro[3];
		sample.getAccelerometer(acc);
		sample.getGyrometer(gyro);
		m_motion->SetValues({acc[0] / 2, acc[1] / 2, acc[2] / 2, gyro[0] / 8, gyro[1] / 8, gyro[2] / 8});
	}
	else
		m_motion->SetInactive(_("No motion device (mouse fallback)"));

	if (emulated_controller->has_position())
	{
		const auto p = emulated_controller->get_position();
		m_touch->SetTouch(std::array<float, 2>{p.x, p.y});
	}
	else
		m_touch->SetTouch(std::nullopt);
}

void VPADInputPanel::load_controller(const EmulatedControllerPtr& controller)
{
	InputPanel::load_controller(controller);
	if (controller)
		m_togglePadViewCheckBox->SetValue(static_cast<VPADController*>(controller.get())->is_screen_active_toggle());
}
