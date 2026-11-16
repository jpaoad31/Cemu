#pragma once

// Feeds a game window's mouse (and touch) events to the Mouse device (input/MouseState.h; fork, see
// INPUT_REWORK.md). The handlers skip the events, so the windows' own handlers still run.

#include <wx/window.h>
#include <wx/event.h>

#include "input/MouseState.h"

inline void BindMouseFeed(wxWindow* canvas, MouseState::Source source)
{
	const auto pos = [canvas](const wxPoint& p) { return canvas->ToPhys(p); };
	canvas->Bind(wxEVT_MOTION, [source, pos](wxMouseEvent& event) {
		const auto p = pos(event.GetPosition());
		MouseState::Get().on_move(source, p.x, p.y);
		event.Skip();
	});
	const auto button = [source, pos](int index, bool down) {
		return [source, pos, index, down](wxMouseEvent& event) {
			const auto p = pos(event.GetPosition());
			MouseState::Get().on_button(source, index, down, p.x, p.y);
			event.Skip();
		};
	};
	canvas->Bind(wxEVT_LEFT_DOWN, button(MouseInputs::kLeft, true));
	canvas->Bind(wxEVT_LEFT_DCLICK, button(MouseInputs::kLeft, true));
	canvas->Bind(wxEVT_LEFT_UP, button(MouseInputs::kLeft, false));
	canvas->Bind(wxEVT_RIGHT_DOWN, button(MouseInputs::kRight, true));
	canvas->Bind(wxEVT_RIGHT_DCLICK, button(MouseInputs::kRight, true));
	canvas->Bind(wxEVT_RIGHT_UP, button(MouseInputs::kRight, false));
	canvas->Bind(wxEVT_MIDDLE_DOWN, button(MouseInputs::kMiddle, true));
	canvas->Bind(wxEVT_MIDDLE_DCLICK, button(MouseInputs::kMiddle, true));
	canvas->Bind(wxEVT_MIDDLE_UP, button(MouseInputs::kMiddle, false));
	canvas->Bind(wxEVT_AUX1_DOWN, button(MouseInputs::kBack, true));
	canvas->Bind(wxEVT_AUX1_DCLICK, button(MouseInputs::kBack, true));
	canvas->Bind(wxEVT_AUX1_UP, button(MouseInputs::kBack, false));
	canvas->Bind(wxEVT_AUX2_DOWN, button(MouseInputs::kForward, true));
	canvas->Bind(wxEVT_AUX2_DCLICK, button(MouseInputs::kForward, true));
	canvas->Bind(wxEVT_AUX2_UP, button(MouseInputs::kForward, false));
	canvas->Bind(wxEVT_MOUSEWHEEL, [source](wxMouseEvent& event) {
		if (event.GetWheelAxis() == wxMOUSE_WHEEL_VERTICAL && event.GetWheelDelta() > 0)
			MouseState::Get().on_wheel(source, (float)event.GetWheelRotation() / (float)event.GetWheelDelta());
		event.Skip();
	});
	// a touch screen's pan gesture is a left button press at the touch point
	canvas->Bind(wxEVT_GESTURE_PAN, [source, pos](wxPanGestureEvent& event) {
		const auto p = pos(event.GetPosition());
		MouseState::Get().on_button(source, MouseInputs::kLeft, event.IsGestureStart() || !event.IsGestureEnd(), p.x, p.y);
		event.Skip();
	});
}
