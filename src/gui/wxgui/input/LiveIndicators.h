#pragma once

// Live indicators for the input settings (fork addition, after Dolphin's MappingIndicator):
// labelled bars for motion values, and a touch-screen box. Sticks use wxInputDraw.

#include <wx/window.h>
#include <array>
#include <optional>

// bars for values in -1..1 (centred) with a label each
class wxValueBars : public wxWindow
{
public:
	wxValueBars(wxWindow* parent, std::vector<wxString> labels);
	void SetValues(const std::vector<float>& values); // -1..1
	void SetInactive(const wxString& text);           // shows text instead of the bars

private:
	void OnPaint(wxPaintEvent& event);
	std::vector<wxString> m_labels;
	std::vector<float> m_values;
	wxString m_inactive;
};

// the GamePad touch screen: a 16:9 box with the touch point
class wxTouchDraw : public wxWindow
{
public:
	wxTouchDraw(wxWindow* parent);
	void SetTouch(std::optional<std::array<float, 2>> position); // 0..1, top left origin

private:
	void OnPaint(wxPaintEvent& event);
	std::optional<std::array<float, 2>> m_position;
};
