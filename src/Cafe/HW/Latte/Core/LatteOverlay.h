#pragma once

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);
void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);

// fork: resolution readout. Called on the GPU thread (scan-buffer copy / backbuffer draw), like the overlay itself.
// screen: 0 = TV, 1 = GamePad (DRC)
void LatteOverlay_noteScanSource(int screen, uint32 width, uint32 height, uint32 format, sint32 effectiveWidth, sint32 effectiveHeight);
void LatteOverlay_notePresented(int screen, bool padWindow, sint32 width, sint32 height);
