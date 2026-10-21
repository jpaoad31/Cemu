#pragma once

// Adds a MetalView as a subview of `handle` (an NSView*) and returns its CAMetalLayer.
// Both the layer and the view (`outView`) are returned retained; release them with DestroyMetalLayer.
void* CreateMetalLayer(void* handle, float& scaleX, float& scaleY, void** outView);

// Removes the view from its superview and releases the view and the layer (on the main thread:
// called from another thread, it is queued there).
void DestroyMetalLayer(void* layer, void* view);
