#include "wxgui/input/panels/InputPanel.h"

#include <wx/menu.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/wupdlock.h>

#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/input/InputDetector.h"
#include "wxgui/input/ExpressionDialog.h"

InputPanel::InputPanel(wxWindow* parent)
	: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxNO_BORDER)
{
}

InputPanel::~InputPanel()
{
	if (auto* detector = InputDetector::Get())
	{
		for (auto* c : m_controls)
			detector->forget_control(c);
	}
}

MappingControl* InputPanel::create_mapping_control(wxWindow* parent, uint64 mapping, const wxString& label, bool analogue)
{
	auto* control = new MappingControl(parent, mapping, analogue);
	control->set_action_handler([this](MappingControl* c, MappingControl::Action a) { on_mapping_action(c, a); });
	m_controls.emplace_back(control);
	if (!label.empty())
		m_labels[mapping] = label;
	return control;
}

wxStaticBoxSizer* InputPanel::create_group(wxWindow* parent, const wxString& title, const std::vector<std::pair<uint64, wxString>>& rows, bool analogue, wxFlexGridSizer** grid_out)
{
	auto* box = new wxStaticBoxSizer(wxVERTICAL, parent, title);
	auto* grid = new wxFlexGridSizer(2, FromDIP(4), FromDIP(6));
	grid->AddGrowableCol(1, 1);
	for (const auto& [id, label] : rows)
	{
		grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
		grid->Add(create_mapping_control(box->GetStaticBox(), id, label, analogue), 1, wxEXPAND);
	}
	box->Add(grid, 0, wxEXPAND | wxALL, FromDIP(4));
	if (grid_out)
		*grid_out = grid;
	return box;
}

wxButton* InputPanel::create_clear_button(wxWindow* parent, std::vector<uint64> ids, const wxString& what)
{
	auto* button = new wxButton(parent, wxID_ANY, _("Clear"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	button->SetToolTip(wxString::Format(_("Remove all bindings of %s (nothing drives it then)."), what));
	button->Bind(wxEVT_BUTTON, [this, ids, what](wxCommandEvent&) {
		if (!m_controller)
			return;
		for (const auto id : ids)
			m_controller->delete_mapping(id);
		refresh_bindings();
		report(wxString::Format(_("%s: bindings cleared."), what));
	});
	return button;
}

void InputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr&)
{
	if (!emulated_controller)
		return;
	for (auto* control : m_controls)
	{
		if (!control->IsShownOnScreen())
			continue;
		const auto id = control->mapping();
		control->set_live(emulated_controller->get_mapping_value(id), emulated_controller->is_mapping_down(id));
	}
}

void InputPanel::reset_configuration()
{
	wxWindowUpdateLocker lock(this);
	for (auto* control : m_controls)
	{
		control->set_binding({}, {}, {});
		control->set_live(0, false);
	}
}

void InputPanel::load_controller(const EmulatedControllerPtr& controller)
{
	m_controller = controller;
	refresh_bindings();
}

void InputPanel::refresh_bindings()
{
	if (!m_controller)
	{
		reset_configuration();
		return;
	}
	wxWindowUpdateLocker lock(this);
	for (auto* control : m_controls)
	{
		const auto id = control->mapping();
		const auto warnings = m_controller->get_mapping_warnings(id);
		control->set_binding(wxString::FromUTF8(m_controller->get_mapping_name(id)),
			wxString::FromUTF8(m_controller->get_mapping_expression(id)),
			wxString::FromUTF8(fmt::format("{}", fmt::join(warnings, "\n"))),
			m_controller->is_mapping_explicit(id));
	}
}

void InputPanel::report(const wxString& text, bool error)
{
	if (m_status_handler)
		m_status_handler(text, error);
}

void InputPanel::on_mapping_action(MappingControl* control, MappingControl::Action action)
{
	if (!m_controller)
	{
		report(_("Choose an emulated controller first."), true);
		return;
	}
	auto* detector = InputDetector::Get();
	switch (action)
	{
	case MappingControl::Action::Detect:
	case MappingControl::Action::DetectAppend:
	{
		if (!detector)
			return;
		// a second click on the field that is waiting cancels
		if (detector->is_active() && detector->control() == control)
		{
			detector->cancel(_("Detection cancelled.").utf8_string());
			return;
		}
		const bool append = action == MappingControl::Action::DetectAppend;
		const wxString label = m_labels.contains(control->mapping()) ? m_labels[control->mapping()] : wxString();
		const auto device = m_controller->get_device();
		const bool all = detector->all_devices();
		const wxString where = all ? _("any device") : device ? wxString::FromUTF8(device->display_name()) : _("the device");
		report(append ? wxString::Format(_("%s: press an input on %s to add it (3 s, Esc cancels)"), label, where)
			: wxString::Format(_("%s: press an input on %s (3 s, Esc cancels; Shift-click adds instead of replacing)"), label, where));
		detector->start(m_controller, control->mapping(), append, control, [this, label](const InputDetector::Result& result) {
			refresh_bindings();
			if (result.other_device && m_devices_changed_handler)
				m_devices_changed_handler();
			if (result.detected)
				report(wxString::Format(_("%s: %s"), label, wxString::FromUTF8(result.message)));
			else
				report(wxString::FromUTF8(result.message), !result.message.empty());
		}, all);
		break;
	}
	case MappingControl::Action::Menu:
		if (detector && detector->is_active())
			detector->cancel();
		show_menu(control);
		break;
	}
}

void InputPanel::show_menu(MappingControl* control)
{
	const auto id = control->mapping();
	const auto expr = m_controller->parse_expression(m_controller->get_mapping_expression(id));
	const auto leaves = expr.leaves();
	const auto devices = m_controller->get_controllers();

	wxMenu menu;
	enum
	{
		kDetect = 1,
		kDetectAppend,
		kClear,
		kAdvanced,
		kRemoveFirst = 100,
	};
	menu.Append(kDetect, _("Detect (replace)"));
	menu.Append(kDetectAppend, _("Detect and add\tShift-click"));
	menu.AppendSeparator();
	if (expr.is_list() && leaves.size() > 1)
	{
		for (size_t i = 0; i < leaves.size(); ++i)
		{
			const auto& l = leaves[i];
			std::string name = l.input_text;
			if (l.device < devices.size() && l.input)
				name = devices[l.device]->get_button_name(*l.input);
			if (l.qualified)
				name += fmt::format(" [{}]", l.device < devices.size() ? devices[l.device]->display_name() : l.device_text);
			menu.Append(kRemoveFirst + (int)i, wxString::Format(_("Remove %s"), wxString::FromUTF8(name)));
		}
	}
	menu.Append(kClear, _("Clear"));
	menu.Enable(kClear, !expr.empty());
	menu.AppendSeparator();
	menu.Append(kAdvanced, _("Advanced..."));

	const int choice = control->GetPopupMenuSelectionFromUser(menu);
	switch (choice)
	{
	case kDetect:
		on_mapping_action(control, MappingControl::Action::Detect);
		return;
	case kDetectAppend:
		on_mapping_action(control, MappingControl::Action::DetectAppend);
		return;
	case kClear:
		m_controller->delete_mapping(id);
		break;
	case kAdvanced:
		open_advanced(control);
		return;
	default:
		if (choice >= kRemoveFirst && choice < kRemoveFirst + (int)leaves.size())
		{
			std::vector<std::string> kept;
			for (size_t i = 0; i < leaves.size(); ++i)
			{
				if ((int)i != choice - kRemoveFirst)
					kept.emplace_back(InputExpr::quote_input(leaves[i].device_text, leaves[i].input_text));
			}
			m_controller->set_mapping_expression(id, fmt::format("{}", fmt::join(kept, " | ")));
		}
		else
			return;
	}
	refresh_bindings();
}

void InputPanel::open_advanced(MappingControl* control)
{
	const auto id = control->mapping();
	const wxString label = m_labels.contains(id) ? m_labels[id] : wxString::Format("%llu", (unsigned long long)id);
	const size_t device_count = m_controller->get_extra_devices().size();
	ExpressionDialog dialog(wxGetTopLevelParent(this), m_controller, id, label);
	if (dialog.ShowModal() == wxID_OK)
	{
		if (const auto error = m_controller->set_mapping_expression(id, dialog.get_expression()))
			report(wxString::FromUTF8(*error), true);
		else
			report(wxString::Format(_("%s: binding changed."), label));
	}
	refresh_bindings();
	if (m_controller->get_extra_devices().size() != device_count && m_devices_changed_handler)
		m_devices_changed_handler();
}
