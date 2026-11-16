#pragma once

// One binding field in the input settings (fork addition, see INPUT_REWORK.md). Shows the
// binding's inputs, highlights while the binding is active and fills like a bar with analogue
// values. Click: detect (replace); Shift/Option/Command-click: detect and add; right click: menu.
// Drawn by hand (no wxTextCtrl): capture doesn't depend on a text field holding the focus.

#include <wx/window.h>
#include <functional>

class MappingControl : public wxWindow
{
public:
	enum class Action
	{
		Detect,       // replace the binding with the next input
		DetectAppend, // add the next input to the binding
		Menu,
	};

	MappingControl(wxWindow* parent, uint64 mapping, bool analogue);

	uint64 mapping() const { return m_mapping; }
	bool is_analogue() const { return m_analogue; }

	void set_action_handler(std::function<void(MappingControl*, Action)> handler) { m_handler = std::move(handler); }

	// binding text and warnings (the tooltip shows the full expression)
	void set_binding(const wxString& display, const wxString& expression, const wxString& warnings);
	// live state; repaints only on a visible change
	void set_live(float value, bool active);
	// detection countdown; seconds_left < 0 ends detection
	void set_detecting(int seconds_left, bool append);

	wxSize DoGetBestClientSize() const override;
	bool AcceptsFocus() const override { return false; }

private:
	void on_paint(wxPaintEvent& event);
	void on_left_down(wxMouseEvent& event);
	void on_right_down(wxMouseEvent& event);

	uint64 m_mapping;
	bool m_analogue;
	std::function<void(MappingControl*, Action)> m_handler;

	wxString m_display;
	bool m_has_warnings = false;
	float m_value = 0;
	bool m_active = false;
	int m_detect_seconds = -1;
	bool m_detect_append = false;
};
