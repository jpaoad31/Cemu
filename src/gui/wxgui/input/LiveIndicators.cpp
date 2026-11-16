#include "wxgui/input/LiveIndicators.h"

#include <wx/dcbuffer.h>
#include <wx/settings.h>

wxValueBars::wxValueBars(wxWindow* parent, std::vector<wxString> labels)
	: wxWindow(parent, wxID_ANY), m_labels(std::move(labels)), m_values(m_labels.size(), 0.0f)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetMinSize(FromDIP(wxSize(160, (int)m_labels.size() * 14 + 2)));
	Bind(wxEVT_PAINT, &wxValueBars::OnPaint, this);
}

void wxValueBars::SetValues(const std::vector<float>& values)
{
	bool changed = !m_inactive.empty();
	m_inactive.clear();
	for (size_t i = 0; i < m_values.size() && i < values.size(); ++i)
	{
		const float v = std::clamp(values[i], -1.0f, 1.0f);
		changed |= std::abs(v - m_values[i]) > 0.01f;
		m_values[i] = v;
	}
	if (changed)
		Refresh(false);
}

void wxValueBars::SetInactive(const wxString& text)
{
	if (m_inactive == text)
		return;
	m_inactive = text;
	Refresh(false);
}

void wxValueBars::OnPaint(wxPaintEvent&)
{
	wxAutoBufferedPaintDC dc(this);
	dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
	dc.Clear();
	dc.SetFont(GetFont().Smaller());
	const wxSize size = GetClientSize();
	const wxColour text = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
	const wxColour grey = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
	const wxColour bar = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
	if (!m_inactive.empty())
	{
		dc.SetTextForeground(grey);
		dc.DrawText(m_inactive, 0, 0);
		return;
	}
	const int row = size.y / std::max<int>(1, (int)m_labels.size());
	const int label_w = FromDIP(52);
	for (size_t i = 0; i < m_labels.size(); ++i)
	{
		const int y = (int)i * row;
		dc.SetTextForeground(text);
		dc.DrawText(m_labels[i], 0, y);
		const wxRect track(label_w, y + 2, size.x - label_w - 1, row - 4);
		dc.SetPen(wxPen(grey));
		dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
		dc.DrawRectangle(track);
		const int mid = track.x + track.width / 2;
		const int len = (int)(m_values[i] * (track.width / 2));
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(bar));
		if (len >= 0)
			dc.DrawRectangle(mid, track.y + 1, std::max(1, len), track.height - 2);
		else
			dc.DrawRectangle(mid + len, track.y + 1, -len, track.height - 2);
		dc.SetPen(wxPen(grey));
		dc.DrawLine(mid, track.y, mid, track.y + track.height);
	}
}

wxTouchDraw::wxTouchDraw(wxWindow* parent)
	: wxWindow(parent, wxID_ANY)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetMinSize(FromDIP(wxSize(96, 54)));
	Bind(wxEVT_PAINT, &wxTouchDraw::OnPaint, this);
}

void wxTouchDraw::SetTouch(std::optional<std::array<float, 2>> position)
{
	if (position == m_position)
		return;
	m_position = position;
	Refresh(false);
}

void wxTouchDraw::OnPaint(wxPaintEvent&)
{
	wxAutoBufferedPaintDC dc(this);
	dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
	dc.Clear();
	const wxSize size = GetClientSize();
	int w = size.x, h = size.x * 9 / 16;
	if (h > size.y)
	{
		h = size.y;
		w = h * 16 / 9;
	}
	const wxRect screen(0, 0, w - 1, h - 1);
	dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT)));
	dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
	dc.DrawRectangle(screen);
	if (m_position)
	{
		const int x = screen.x + (int)((*m_position)[0] * screen.width);
		const int y = screen.y + (int)((*m_position)[1] * screen.height);
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)));
		dc.DrawCircle(x, y, FromDIP(4));
	}
}
