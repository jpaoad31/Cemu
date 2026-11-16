#pragma once

// "Advanced..." binding editor (fork addition, after Dolphin's IOWindow): the binding as an
// expression, validated while typing, with its live value, and a list of the attached devices'
// inputs to insert. See InputExpression.h for the syntax.

#include <wx/dialog.h>
#include "input/emulated/EmulatedController.h"

class wxTextCtrl;
class wxStaticText;
class wxChoice;
class wxListBox;
class wxTimer;
class wxTimerEvent;
class wxGauge;

class ExpressionDialog : public wxDialog
{
public:
	ExpressionDialog(wxWindow* parent, const EmulatedControllerPtr& controller, uint64 mapping, const wxString& control_name);
	~ExpressionDialog() override;

	std::string get_expression() const;

private:
	void fill_inputs();
	void insert(const wxString& text);
	void on_timer(wxTimerEvent& event);
	void on_ok(wxCommandEvent& event);

	EmulatedControllerPtr m_controller;
	uint64 m_mapping;

	wxTextCtrl* m_text;
	wxStaticText* m_status;
	wxGauge* m_value;
	wxChoice* m_devices;
	wxListBox* m_inputs;
	wxTimer* m_timer;
	std::vector<uint64> m_input_ids;

	wxString m_parsed_text;
	InputExpr::Expression m_expression;
	bool m_valid = true;
};
