#include "Cafe/HW/Latte/Renderer/Metal/MetalLayer.h"

#include "Cafe/HW/Latte/Renderer/MetalView.h"

void* CreateMetalLayer(void* handle, float& scaleX, float& scaleY, void** outView)
{
	NSView* view = (NSView*)handle;

	MetalView* childView = [[MetalView alloc] initWithFrame:view.bounds]; // +1, owned by the caller (outView)
	childView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	childView.wantsLayer = YES;

	[view addSubview:childView];

	const NSRect points = [childView frame];
    const NSRect pixels = [childView convertRectToBacking:points];

	scaleX = (float)(pixels.size.width / points.size.width);
    scaleY = (float)(pixels.size.height / points.size.height);

	*outView = childView;
	// the view owns its layer; retain it so the caller's reference stays valid until DestroyMetalLayer
	return [childView.layer retain];
}

void DestroyMetalLayer(void* layer, void* view)
{
	MetalView* childView = (MetalView*)view;
	CALayer* metalLayer = (CALayer*)layer;
	auto destroy = ^{
		if (childView)
		{
			[childView removeFromSuperview];
			[childView release];
		}
		if (metalLayer)
			[metalLayer release];
	};
	// the renderer (which owns the layer handles) is destroyed on the GPU thread, but AppKit views
	// may only be touched on the main thread
	if ([NSThread isMainThread])
		destroy();
	else
		dispatch_async(dispatch_get_main_queue(), destroy);
}
