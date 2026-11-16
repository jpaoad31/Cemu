#include "input/api/Mouse/MouseInputs.h"
#include "wxgui/input/ExpressionDialog.h"

#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/listbox.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <wx/gauge.h>
#include <wx/settings.h>

#include "input/api/Controller.h"
#include "input/ControllerFactory.h"
#include "wxgui/input/InputDetector.h"

ExpressionDialog::ExpressionDialog(wxWindow* parent, const EmulatedControllerPtr& controller, uint64 mapping, const wxString& control_name)
	: wxDialog(parent, wxID_ANY, wxString::Format(_("Binding: %s"), control_name), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
	  m_controller(controller), m_mapping(mapping)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	auto* help = new wxStaticText(this, wxID_ANY, _("Inputs are written `Input` (this controller's device) or `Device:Input` (another device, explicitly: an advanced, deliberate choice). Pick them below. Operators: | (either), & (both), ! (not), ^, + - * /, < >, parentheses.\nFunctions: min, max, clamp, abs, deadzone(x, dz), toggle(x), hold(x, seconds), pulse(x, seconds), timer(seconds).\nA button is pressed while the result is active (an input pressed, or a computed value above 0.5); sticks and triggers use the value."));
	help->Wrap(FromDIP(620));
	sizer->Add(help, 0, wxALL, 8);

	m_text = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(controller->get_mapping_expression(mapping)), wxDefaultPosition, FromDIP(wxSize(620, 90)), wxTE_MULTILINE);
	m_text->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
	sizer->Add(m_text, 1, wxLEFT | wxRIGHT | wxEXPAND, 8);

	auto* status_sizer = new wxBoxSizer(wxHORIZONTAL);
	m_value = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, FromDIP(wxSize(120, 14)));
	status_sizer->Add(m_value, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
	status_sizer->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
	sizer->Add(status_sizer, 0, wxALL | wxEXPAND, 8);

	// insert helpers
	auto* insert_sizer = new wxBoxSizer(wxHORIZONTAL);
	auto* left = new wxBoxSizer(wxVERTICAL);
	m_devices = new wxChoice(this, wxID_ANY);
	fill_devices();
	m_devices->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { fill_inputs(); });
	left->Add(m_devices, 0, wxEXPAND | wxBOTTOM, 4);
	m_inputs = new wxListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(260, 150)));
	m_inputs->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
		const int sel = m_inputs->GetSelection();
		const int dev = m_devices->GetSelection();
		if (sel == wxNOT_FOUND || dev == wxNOT_FOUND || dev >= (int)m_device_list.size())
			return;
		insert(input_text(m_device_list[dev], m_input_ids[sel]));
	});
	left->Add(m_inputs, 1, wxEXPAND);
	insert_sizer->Add(left, 1, wxEXPAND | wxRIGHT, 8);

	auto* ops = new wxBoxSizer(wxVERTICAL);
	auto* detect = new wxButton(this, wxID_ANY, _("Detect input"));
	detect->SetToolTip(_("Press an input on any device within 3 s; it is inserted at the cursor (another device than this controller's is written with its name)."));
	detect->Bind(wxEVT_BUTTON, [this, detect](wxCommandEvent&) {
		auto* detector = InputDetector::Get();
		if (!detector)
			return;
		detect->SetLabel(_("Press an input..."));
		detector->start_for_input(m_controller,
			[this](const ControllerPtr& c, uint64 input) { insert(input_text(c, input)); },
			[this, detect](const InputDetector::Result& result) {
				detect->SetLabel(_("Detect input"));
				fill_devices();
				fill_inputs();
				if (!result.message.empty())
					m_status->SetLabel(wxString::FromUTF8(result.message));
			});
	});
	ops->Add(detect, 0, wxEXPAND | wxBOTTOM, 6);
	auto* insert_input = new wxButton(this, wxID_ANY, _("Insert selected input"));
	insert_input->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		wxCommandEvent e(wxEVT_LISTBOX_DCLICK, m_inputs->GetId());
		m_inputs->GetEventHandler()->ProcessEvent(e);
	});
	ops->Add(insert_input, 0, wxEXPAND | wxBOTTOM, 6);
	auto* op_row = new wxGridSizer(4, 2, 2);
	for (const char* op : {"|", "&", "!", "^", "(", ")", "+", "-"})
	{
		auto* b = new wxButton(this, wxID_ANY, op, wxDefaultPosition, FromDIP(wxSize(34, -1)), wxBU_EXACTFIT);
		b->Bind(wxEVT_BUTTON, [this, op](wxCommandEvent&) {
			const std::string_view o = op;
			insert(o == "(" || o == ")" || o == "!" ? wxString(op) : wxString::Format(" %s ", op));
		});
		op_row->Add(b, 0, wxEXPAND);
	}
	ops->Add(op_row, 0, wxEXPAND | wxBOTTOM, 6);
	auto* clear = new wxButton(this, wxID_ANY, _("Clear"));
	clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_text->Clear(); });
	ops->Add(clear, 0, wxEXPAND);
	insert_sizer->Add(ops, 0, wxEXPAND);
	sizer->Add(insert_sizer, 1, wxLEFT | wxRIGHT | wxEXPAND, 8);

	auto* buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
	sizer->Add(buttons, 0, wxALL | wxEXPAND, 8);
	Bind(wxEVT_BUTTON, &ExpressionDialog::on_ok, this, wxID_OK);

	SetSizerAndFit(sizer);
	SetMinSize(GetSize());
	CentreOnParent();
	fill_inputs();

	// keys pressed while "Detect input" waits go to the detector, not the text field
	Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent& event) {
		if (auto* detector = InputDetector::Get(); detector && detector->on_char_hook(event))
			return;
		event.Skip();
	});

	m_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, &ExpressionDialog::on_timer, this, m_timer->GetId());
	m_timer->Start(50);
	m_text->SetFocus();
	m_text->SetInsertionPointEnd();
}

ExpressionDialog::~ExpressionDialog()
{
	m_timer->Stop();
	if (auto* detector = InputDetector::Get(); detector && detector->is_active())
		detector->cancel();
}

std::string ExpressionDialog::get_expression() const
{
	return m_text->GetValue().utf8_string();
}

wxString ExpressionDialog::input_text(const ControllerPtr& device, uint64 input) const
{
	const auto selected = m_controller->get_device();
	const auto name = device->get_button_name(input);
	if (selected && *selected == *device)
		return wxString::FromUTF8(InputExpr::quote_input({}, name));
	return wxString::FromUTF8(InputExpr::quote_input(fmt::format("{}/{}", InputAPI::to_string(device->api()), device->uuid()), name));
}

void ExpressionDialog::fill_devices()
{
	// the controller's device, the devices bindings name, and Keyboard/Mouse
	std::vector<ControllerPtr> list;
	const auto add = [&](const ControllerPtr& c) {
		if (c && std::none_of(list.cbegin(), list.cend(), [&](const ControllerPtr& l) { return *l == *c; }))
			list.push_back(c);
	};
	const auto selected = m_controller->get_device();
	add(selected);
	for (const auto& c : m_controller->get_extra_devices())
		add(c);
	try
	{
		add(ControllerFactory::CreateController(InputAPI::Keyboard, "keyboard", "Keyboard/Mouse"));
	}
	catch (const std::exception&)
	{
	}
	if (list.size() == m_device_list.size() && std::equal(list.cbegin(), list.cend(), m_device_list.cbegin(), [](const auto& a, const auto& b) { return *a == *b; }))
		return;
	const int sel = m_devices->GetSelection();
	m_device_list = std::move(list);
	m_devices->Clear();
	for (const auto& c : m_device_list)
	{
		const bool is_selected = selected && *selected == *c;
		m_devices->Append(wxString::FromUTF8(is_selected ? fmt::format("{} (this controller's device)", c->display_name())
			: fmt::format("{} [{}] (explicit)", c->display_name(), c->api_name())));
	}
	m_devices->SetSelection(sel == wxNOT_FOUND || sel >= (int)m_device_list.size() ? 0 : sel);
}

void ExpressionDialog::fill_inputs()
{
	m_inputs->Clear();
	m_input_ids.clear();
	const int dev = m_devices->GetSelection();
	if (dev == wxNOT_FOUND || dev >= (int)m_device_list.size())
		return;
	const auto& c = m_device_list[dev];
	const bool keyboard = c->api() == InputAPI::Keyboard;
	const uint64 limit = keyboard ? 0x200 : kButtonMAX;
	wxArrayString names;
	for (uint64 id = 0; id < limit; ++id)
	{
		const auto name = c->get_button_name(id);
		if (keyboard && (name.empty() || name.starts_with("key_")))
			continue;
		names.Add(wxString::FromUTF8(name));
		m_input_ids.emplace_back(id);
	}
	if (keyboard)
	{
		// the mouse part of Keyboard/Mouse
		for (const auto id : MouseInputs::AllIds())
		{
			names.Add(wxString::FromUTF8(c->get_button_name(MouseInputs::KeyboardId(id))));
			m_input_ids.emplace_back(MouseInputs::KeyboardId(id));
		}
	}
	m_inputs->Append(names);
}

void ExpressionDialog::insert(const wxString& text)
{
	m_text->WriteText(text);
	m_text->SetFocus();
}

void ExpressionDialog::on_timer(wxTimerEvent&)
{
	if (auto* detector = InputDetector::Get())
		detector->poll();

	const auto text = m_text->GetValue();
	if (text != m_parsed_text)
	{
		m_parsed_text = text;
		m_expression = m_controller->parse_expression(text.utf8_string());
		m_valid = m_expression.ok();
	}

	for (const auto& c : m_controller->get_controllers())
		InputDetector::update_device(*c);
	const auto [value, active] = m_controller->evaluate_expression(m_expression);
	m_value->SetValue((int)(std::clamp(std::abs(value), 0.0f, 1.0f) * 100));

	if (auto* detector = InputDetector::Get(); detector && detector->is_active())
		return; // the status line shows the detect result when it ends

	wxString status;
	wxColour colour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	if (!m_valid)
	{
		status = wxString::FromUTF8(m_expression.error());
		colour = wxTheColourDatabase->Find("ERROR");
	}
	else if (m_expression.empty())
		status = _("Empty: not bound.");
	else
	{
		status = wxString::Format(_("Value %.2f, %s"), value, active ? _("ACTIVE") : _("not active"));
		const auto warnings = m_expression.warnings();
		if (!warnings.empty())
		{
			status += " - " + wxString::FromUTF8(fmt::format("{}", fmt::join(warnings, "; ")));
			colour = wxTheColourDatabase->Find("ERROR");
		}
	}
	if (m_status->GetLabel() != status)
		m_status->SetLabel(status);
	if (m_status->GetForegroundColour() != colour)
		m_status->SetForegroundColour(colour);
}

void ExpressionDialog::on_ok(wxCommandEvent& event)
{
	const auto expr = m_controller->parse_expression(get_expression());
	if (!expr.ok())
	{
		wxBell();
		m_text->SetFocus();
		return;
	}
	EndModal(wxID_OK);
}
