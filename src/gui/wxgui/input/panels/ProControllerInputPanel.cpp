#include "wxgui/input/panels/ProControllerInputPanel.h"

#include <wx/sizer.h>
#include <wx/statbox.h>

#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/components/wxInputDraw.h"

namespace
{
	using C = ProController;
	wxString name(C::ButtonId id)
	{
		return wxGetTranslation(wxString::FromUTF8(C::get_button_name(id)));
	}
}

ProControllerInputPanel::ProControllerInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	auto* main_sizer = new wxBoxSizer(wxHORIZONTAL);
	const int gap = FromDIP(4);

	auto* col1 = new wxBoxSizer(wxVERTICAL);
	col1->Add(create_group(this, _("Buttons"), {
		{C::kButtonId_A, "A"}, {C::kButtonId_B, "B"}, {C::kButtonId_X, "X"}, {C::kButtonId_Y, "Y"},
		{C::kButtonId_L, "L"}, {C::kButtonId_R, "R"}, {C::kButtonId_ZL, "ZL"}, {C::kButtonId_ZR, "ZR"},
		{C::kButtonId_Plus, "+"}, {C::kButtonId_Minus, "-"}, {C::kButtonId_Home, _("Home")},
	}), 0, wxEXPAND | wxALL, gap);
	main_sizer->Add(col1, 1, wxEXPAND);

	auto* col2 = new wxBoxSizer(wxVERTICAL);
	auto* left = create_group(this, _("Left stick"), {
		{C::kButtonId_StickL_Up, name(C::kButtonId_StickL_Up)}, {C::kButtonId_StickL_Down, name(C::kButtonId_StickL_Down)},
		{C::kButtonId_StickL_Left, name(C::kButtonId_StickL_Left)}, {C::kButtonId_StickL_Right, name(C::kButtonId_StickL_Right)},
		{C::kButtonId_StickL, name(C::kButtonId_StickL)},
	}, true);
	m_left_draw = new wxInputDraw(left->GetStaticBox(), wxID_ANY, wxDefaultPosition, FromDIP(wxSize(64, 64)));
	left->Add(m_left_draw, 0, wxALIGN_CENTER | wxALL, gap);
	col2->Add(left, 0, wxEXPAND | wxALL, gap);
	col2->Add(create_group(this, _("D-pad"), {
		{C::kButtonId_Up, name(C::kButtonId_Up)}, {C::kButtonId_Down, name(C::kButtonId_Down)},
		{C::kButtonId_Left, name(C::kButtonId_Left)}, {C::kButtonId_Right, name(C::kButtonId_Right)},
	}), 0, wxEXPAND | wxALL, gap);
	main_sizer->Add(col2, 1, wxEXPAND);

	auto* col3 = new wxBoxSizer(wxVERTICAL);
	auto* right = create_group(this, _("Right stick"), {
		{C::kButtonId_StickR_Up, name(C::kButtonId_StickR_Up)}, {C::kButtonId_StickR_Down, name(C::kButtonId_StickR_Down)},
		{C::kButtonId_StickR_Left, name(C::kButtonId_StickR_Left)}, {C::kButtonId_StickR_Right, name(C::kButtonId_StickR_Right)},
		{C::kButtonId_StickR, name(C::kButtonId_StickR)},
	}, true);
	m_right_draw = new wxInputDraw(right->GetStaticBox(), wxID_ANY, wxDefaultPosition, FromDIP(wxSize(64, 64)));
	right->Add(m_right_draw, 0, wxALIGN_CENTER | wxALL, gap);
	col3->Add(right, 0, wxEXPAND | wxALL, gap);
	main_sizer->Add(col3, 1, wxEXPAND);

	SetSizer(main_sizer);
	Layout();
}

void ProControllerInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller_base)
{
	InputPanel::on_timer(emulated_controller, controller_base);
	if (!emulated_controller)
		return;
	m_left_draw->SetAxisValue(emulated_controller->get_axis());
	m_right_draw->SetAxisValue(emulated_controller->get_rotation());
}
