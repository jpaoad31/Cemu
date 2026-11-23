#include "wxgui/FitMeterFrame.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/wxHelper.h"

#include "Cafe/OS/libs/nsysccr/nsysccr.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/utils.h>

FitMeterFrame::FitMeterFrame(wxWindow* parent)
	: wxFrame(parent, wxID_ANY, _("Virtual Fit Meter"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE | wxTAB_TRAVERSAL),
	  m_timer(this)
{
	auto* panel = new wxPanel(this);
	auto* top = new wxBoxSizer(wxVERTICAL);

	// the meter
	{
		auto* box = new wxStaticBoxSizer(wxVERTICAL, panel, _("Meter"));
		auto* sb = box->GetStaticBox();
		m_enabled = new wxCheckBox(sb, wxID_ANY, _("Virtual Fit Meter on (Wii Fit U finds it on the GamePad's IR port)"));
		m_enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { FitMeterHost::SetEnabled(m_enabled->GetValue()); Refresh(true); });
		box->Add(m_enabled, 0, wxALL, 4);
		m_autoPress = new wxCheckBox(sb, wxID_ANY, _("Press the meter's button automatically when the game asks for the meter"));
		m_autoPress->SetToolTip(_("The game asks you to point the Fit Meter at the GamePad and press its button. With this on, the virtual meter connects about 1.5 s after the game starts waiting."));
		m_autoPress->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { FitMeterHost::SetAutoPress(m_autoPress->GetValue()); Refresh(true); });
		box->Add(m_autoPress, 0, wxALL, 4);
		auto* row = new wxBoxSizer(wxHORIZONTAL);
		m_press = new wxButton(sb, wxID_ANY, _("Press meter button"));
		m_press->SetToolTip(_("Connects if the game is waiting for the meter now or within 10 seconds"));
		m_press->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FitMeterHost::PressButton(); Refresh(true); });
		row->Add(m_press, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
		m_link = new wxStaticText(sb, wxID_ANY, "");
		row->Add(m_link, 1, wxALIGN_CENTER_VERTICAL);
		box->Add(row, 0, wxEXPAND | wxALL, 4);
		m_registration = new wxStaticText(sb, wxID_ANY, "");
		box->Add(m_registration, 0, wxEXPAND | wxALL, 4);
		m_clock = new wxStaticText(sb, wxID_ANY, "");
		box->Add(m_clock, 0, wxEXPAND | wxALL, 4);
		m_lastSync = new wxStaticText(sb, wxID_ANY, "");
		box->Add(m_lastSync, 0, wxEXPAND | wxALL, 4);
		auto* reset = new wxButton(sb, wxID_ANY, _("Reset meter (unregister)..."));
		reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (wxMessageBox(_("Make the virtual meter new and unregistered?\n\nThe meter forgets its Mii, ID and settings. Wii Fit U's save still lists it as this user's meter: unregister it in the game too (or register again)."),
					_("Reset meter"), wxYES_NO | wxICON_QUESTION, this) == wxYES)
			{
				FitMeterHost::Unregister();
				Refresh(true);
			}
		});
		box->Add(reset, 0, wxALL, 4);
		top->Add(box, 0, wxEXPAND | wxALL, 6);
	}

	// the data
	{
		auto* box = new wxStaticBoxSizer(wxVERTICAL, panel, _("Activity data"));
		auto* sb = box->GetStaticBox();
		auto* srow = new wxBoxSizer(wxHORIZONTAL);
		srow->Add(new wxStaticText(sb, wxID_ANY, _("Source:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		wxString choices[] = {_("Built-in demo (4-week cycle)"), _("activity.json file")};
		m_source = new wxChoice(sb, wxID_ANY, wxDefaultPosition, wxDefaultSize, 2, choices);
		m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { FitMeterHost::UseDemo(m_source->GetSelection() == 0); Refresh(true); });
		srow->Add(m_source, 0, wxALIGN_CENTER_VERTICAL);
		box->Add(srow, 0, wxALL, 4);
		m_activity = new wxStaticText(sb, wxID_ANY, "");
		box->Add(m_activity, 0, wxEXPAND | wxALL, 4);
		m_hint = new wxStaticText(sb, wxID_ANY, "");
		box->Add(m_hint, 0, wxEXPAND | wxALL, 4);
		auto* row = new wxBoxSizer(wxHORIZONTAL);
		m_override = new wxCheckBox(sb, wxID_ANY, _("Set today's steps:"));
		m_override->SetToolTip(_("Replaces today's step count from the source (the per-minute data stays)"));
		m_override->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
			std::string err;
			if (m_override->GetValue())
				FitMeterHost::SetStepsToday(m_stepsToday->GetValue(), err);
			else
				FitMeterHost::ClearStepsToday();
			Refresh(true);
		});
		row->Add(m_override, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
		m_stepsToday = new wxSpinCtrl(sb, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 65529, 0);
		m_stepsToday->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) {
			std::string err;
			if (m_override->GetValue())
				FitMeterHost::SetStepsToday(m_stepsToday->GetValue(), err);
			Refresh(true);
		});
		row->Add(m_stepsToday, 0, wxALIGN_CENTER_VERTICAL);
		box->Add(row, 0, wxALL, 4);
		auto* row2 = new wxBoxSizer(wxHORIZONTAL);
		auto* load = new wxButton(sb, wxID_ANY, _("Load activity.json..."));
		load->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			wxFileDialog dlg(this, _("Load activity data"), "", "", _("JSON files (*.json)|*.json|All files|*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
			if (dlg.ShowModal() != wxID_OK)
				return;
			std::string err;
			if (!FitMeterHost::ImportActivityFile(wxHelper::MakeFSPath(dlg.GetPath()), err))
				wxMessageBox(wxString::FromUTF8(err), _("Virtual Fit Meter"), wxOK | wxICON_ERROR, this);
			Refresh(true);
		});
		row2->Add(load, 0, wxRIGHT, 6);
		auto* open = new wxButton(sb, wxID_ANY, _("Open folder"));
		open->Bind(wxEVT_BUTTON, [](wxCommandEvent&) { wxLaunchDefaultApplication(wxHelper::FromPath(FitMeterHost::GetFolder())); });
		row2->Add(open, 0);
		box->Add(row2, 0, wxALL, 4);
		m_folder = new wxStaticText(sb, wxID_ANY, "");
		box->Add(m_folder, 0, wxEXPAND | wxALL, 4);
		top->Add(box, 0, wxEXPAND | wxALL, 6);
	}

	panel->SetSizer(top);
	auto* outer = new wxBoxSizer(wxVERTICAL);
	outer->Add(panel, 1, wxEXPAND);
	SetSizer(outer);
	Refresh(true);
	Fit();
	SetMinSize(wxSize(520, GetSize().GetHeight()));

	Bind(wxEVT_TIMER, &FitMeterFrame::OnTimer, this);
	m_timer.Start(500);
}

void FitMeterFrame::OnTimer(wxTimerEvent&)
{
	if (IsShown())
		Refresh();
}

void FitMeterFrame::Refresh(bool force)
{
	const auto s = FitMeterHost::GetStatus();
	m_enabled->SetValue(s.enabled);
	m_autoPress->SetValue(s.autoPress);
	m_press->Enable(s.enabled);
	m_link->SetLabel(wxString::FromUTF8(fmt::format("Link: {} ({} frames in, {} out)", s.link, s.framesIn, s.framesOut)));
	if (s.registered)
		m_registration->SetLabel(wxString::FromUTF8(fmt::format("Registered to: {}{}   (meter ID {:08X}, save {:08X}, since {})",
			s.miiName.empty() ? "(no Mii name)" : s.miiName, s.pet ? " (pet)" : "", s.samuId, s.saveDataId, s.registeredAt)));
	else
		m_registration->SetLabel(_("Not registered: register it in Wii Fit U (Fit Meter > settings)"));
	m_clock->SetLabel(wxString::FromUTF8(fmt::format("Meter clock: {:04}-{:02}-{:02} {:02}:{:02}{}", s.clock.year, s.clock.month, s.clock.day,
		s.clock.hour, s.clock.minute, s.clockSet ? "" : " (not set yet: the game sets it)")));
	m_lastSync->SetLabel(wxString::FromUTF8(s.lastSync.empty() ? std::string("Last sync: never") :
		fmt::format("Last sync: {} ({} so far)", s.lastSync, s.syncCount)));
	m_source->SetSelection(s.useFile ? 1 : 0);
	m_activity->SetLabel(wxString::FromUTF8(fmt::format("Active: {}\n{}\nToday's steps so far: {}", s.source, s.activitySummary,
		s.stepsToday >= 0 ? std::to_string(s.stepsToday) : std::string("none"))));
	m_hint->SetLabel(wxString::FromUTF8(s.hint));
	m_hint->Show(!s.hint.empty());
	m_folder->SetLabel(wxString::FromUTF8("Meter folder: " + s.folder));
	m_override->SetValue(s.override);
	if (force || !m_stepsToday->HasFocus())
		m_stepsToday->SetValue(s.override ? s.overrideSteps : std::max(0, s.stepsToday));
	Layout();
}
