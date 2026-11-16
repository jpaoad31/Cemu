#include "wxgui/input/MappingControl.h"

#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/control.h>

MappingControl::MappingControl(wxWindow* parent, uint64 mapping, bool analogue)
	: wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE | wxBORDER_NONE),
	  m_mapping(mapping), m_analogue(analogue)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetMinSize(FromDIP(wxSize(140, 22)));
	Bind(wxEVT_PAINT, &MappingControl::on_paint, this);
	Bind(wxEVT_LEFT_DOWN, &MappingControl::on_left_down, this);
	Bind(wxEVT_RIGHT_DOWN, &MappingControl::on_right_down, this);
	SetCursor(wxCursor(wxCURSOR_HAND));
}

wxSize MappingControl::DoGetBestClientSize() const
{
	return FromDIP(wxSize(160, 22));
}

void MappingControl::set_binding(const wxString& display, const wxString& expression, const wxString& warnings, bool explicit_device)
{
	const bool has_warnings = !warnings.empty();
	if (display == m_display && has_warnings == m_has_warnings && explicit_device == m_explicit)
		return;
	m_display = display;
	m_has_warnings = has_warnings;
	m_explicit = explicit_device;
	wxString tip = expression.empty() ? _("Not bound.") : expression;
	if (explicit_device)
		tip += "\n\n" + _("Uses another device than this controller's, by name (advanced).");
	if (has_warnings)
		tip += "\n\n" + warnings;
	tip += "\n\n" + _("Click: detect (press an input on this controller's device within 3 s, Esc cancels).\nShift-click: detect and add to the binding.\nRight click: clear, remove one input, advanced editor.");
	SetToolTip(tip);
	Refresh();
}

void MappingControl::set_live(float value, bool active)
{
	value = std::clamp(std::abs(value), 0.0f, 1.0f);
	// repaint on a visible change only (the timer runs at 40 Hz for every field)
	if (active == m_active && std::abs(value - m_value) < 0.02f)
		return;
	m_value = value;
	m_active = active;
	Refresh(false);
}

void MappingControl::set_detecting(int seconds_left, bool append)
{
	if (seconds_left == m_detect_seconds && append == m_detect_append)
		return;
	m_detect_seconds = seconds_left;
	m_detect_append = append;
	Refresh(false);
}

void MappingControl::on_paint(wxPaintEvent&)
{
	wxAutoBufferedPaintDC dc(this);
	const wxSize size = GetClientSize();

	dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
	dc.Clear();

	const wxColour field = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
	const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	const wxColour highlight = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
	const wxColour highlight_text = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
	const wxColour grey = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
	wxColour border = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW);
	if (m_has_warnings)
		border = wxTheColourDatabase->Find("ERROR");
	const bool detecting = m_detect_seconds >= 0;
	if (detecting)
		border = highlight;

	wxRect r(wxPoint(0, 0), size);
	r.Deflate(1);
	const double radius = FromDIP(4);

	dc.SetPen(wxPen(border, detecting ? 2 : 1));
	dc.SetBrush(wxBrush(field));
	dc.DrawRoundedRectangle(r, radius);

	wxColour text_colour = text;
	if (detecting)
	{
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(highlight.ChangeLightness(170)));
		dc.DrawRoundedRectangle(r.Deflate(1), radius);
	}
	else if (m_active)
	{
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(highlight));
		dc.DrawRoundedRectangle(r, radius);
		text_colour = highlight_text;
	}
	else if (m_value > 0.01f)
	{
		// analogue value below the "pressed" threshold: a bar
		wxRect bar = r;
		bar.Deflate(1);
		bar.width = std::max(1, (int)(bar.width * m_value));
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(highlight.ChangeLightness(160)));
		dc.DrawRoundedRectangle(bar, radius);
	}

	wxString label;
	if (detecting)
	{
		label = m_detect_append ? wxString::Format(_("Press an input to add... %d"), m_detect_seconds)
		                        : wxString::Format(_("Press an input... %d"), m_detect_seconds);
		text_colour = text;
	}
	else if (m_display.empty())
	{
		label = wxString::FromUTF8("—"); // em dash: unbound
		if (!m_active)
			text_colour = grey;
	}
	else
		label = m_display;

	const int pad = FromDIP(6);
	if (m_explicit && !detecting)
	{
		// another device, named explicitly: a mark at the left and the link colour
		const wxColour link = wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT);
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(link));
		dc.DrawRectangle(r.x + 1, r.y + FromDIP(3), FromDIP(3), r.height - FromDIP(6));
		if (!m_active)
			text_colour = link;
	}
	dc.SetFont(GetFont());
	dc.SetTextForeground(text_colour);
	const wxString shown = wxControl::Ellipsize(label, dc, wxELLIPSIZE_END, r.width - 2 * pad);
	const wxSize extent = dc.GetTextExtent(shown);
	dc.DrawText(shown, r.x + pad, r.y + (r.height - extent.y) / 2);
}

void MappingControl::on_left_down(wxMouseEvent& event)
{
	if (!m_handler)
		return;
	const bool append = event.ShiftDown() || event.AltDown() || event.CmdDown();
	m_handler(this, append ? Action::DetectAppend : Action::Detect);
}

void MappingControl::on_right_down(wxMouseEvent& event)
{
	if (m_handler)
		m_handler(this, Action::Menu);
}
