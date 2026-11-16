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

// a pointer box (after Dolphin's IR indicator): the GamePad touch screen, the Wii Remote pointer on
// the TV, the board's centre of balance. A box of the given aspect with the point: filled while
// "pressed" (touching, visible), hollow otherwise; nothing when there is no point.
class wxPointerBox : public wxWindow
{
public:
	wxPointerBox(wxWindow* parent, float aspect = 16.0f / 9.0f, bool crosshair = false);
	struct State
	{
		bool has_point = false;
		bool pressed = false;
		float x = 0.5f, y = 0.5f; // 0..1, top left origin
		bool operator==(const State&) const = default;
	};
	void SetState(const State& state);
	void SetCaption(const wxString& caption); // under the box, e.g. the source

private:
	void OnPaint(wxPaintEvent& event);
	float m_aspect;
	bool m_crosshair;
	State m_state;
	wxString m_caption;
};
