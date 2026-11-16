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

wxPointerBox::wxPointerBox(wxWindow* parent, float aspect, bool crosshair)
	: wxWindow(parent, wxID_ANY), m_aspect(aspect), m_crosshair(crosshair)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	const int w = FromDIP(112);
	SetMinSize(wxSize(w, (int)(w / aspect) + FromDIP(14)));
	Bind(wxEVT_PAINT, &wxPointerBox::OnPaint, this);
}

void wxPointerBox::SetState(const State& state)
{
	if (state == m_state)
		return;
	m_state = state;
	Refresh(false);
}

void wxPointerBox::SetCaption(const wxString& caption)
{
	if (caption == m_caption)
		return;
	m_caption = caption;
	Refresh(false);
}

void wxPointerBox::OnPaint(wxPaintEvent&)
{
	wxAutoBufferedPaintDC dc(this);
	dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
	dc.Clear();
	const wxSize size = GetClientSize();
	const int caption_h = FromDIP(14);
	int h = std::max(1, size.y - caption_h), w = (int)(h * m_aspect);
	if (w > size.x)
	{
		w = size.x;
		h = (int)(w / m_aspect);
	}
	const wxRect box((size.x - w) / 2, 0, w - 1, h - 1);
	const wxColour grey = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
	dc.SetPen(wxPen(grey));
	dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)));
	dc.DrawRectangle(box);
	if (m_crosshair)
	{
		dc.SetPen(wxPen(grey, 1, wxPENSTYLE_DOT));
		dc.DrawLine(box.x + box.width / 2, box.y, box.x + box.width / 2, box.y + box.height);
		dc.DrawLine(box.x, box.y + box.height / 2, box.x + box.width, box.y + box.height / 2);
	}
	if (m_state.has_point)
	{
		const int x = box.x + (int)(std::clamp(m_state.x, 0.0f, 1.0f) * box.width);
		const int y = box.y + (int)(std::clamp(m_state.y, 0.0f, 1.0f) * box.height);
		const wxColour accent = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
		dc.SetPen(wxPen(accent, FromDIP(2)));
		dc.SetBrush(m_state.pressed ? wxBrush(accent) : *wxTRANSPARENT_BRUSH);
		dc.DrawCircle(x, y, FromDIP(4));
	}
	if (!m_caption.empty())
	{
		dc.SetFont(GetFont().Smaller());
		dc.SetTextForeground(grey);
		dc.DrawText(m_caption, box.x, box.y + box.height + FromDIP(1));
	}
}
