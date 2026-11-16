// Test helper (fork): draws a window's content view into a PNG, without screen recording
// permission. Used by the CEMU_TEST_INPUT_SETTINGS_SHOT test hook.
#import <Cocoa/Cocoa.h>

bool MacSnapshotView(void* handle, const char* path)
{
	@autoreleasepool
	{
		id object = (__bridge id)handle;
		NSView* view = nil;
		if ([object isKindOfClass:[NSWindow class]])
			view = [(NSWindow*)object contentView];
		else if ([object isKindOfClass:[NSView class]])
			view = [[(NSView*)object window] contentView] ?: (NSView*)object;
		if (!view)
			return false;
		NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
		if (!rep)
			return false;
		[view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
		NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
		return [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
	}
}
