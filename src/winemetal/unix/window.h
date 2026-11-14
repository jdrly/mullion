#ifndef __WINEMETAL_WINDOW_H
#define __WINEMETAL_WINDOW_H

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

// the window lines (docs/swtor/README.md § The window lines): a line on the process's stderr at each change of the
// visibility of the watched view's window, or of the app's activation. both on the main thread

// watches `view`'s window from now on, in place of the one before: the last view a swap chain made. `layer` is the
// layer it presents with, which window_lines_visibility answers for
void window_lines_watch(NSView *view, CAMetalLayer *layer);

// stops watching `view` when it is the one watched; before the view is released
void window_lines_unwatch(NSView *view);

// any thread: true when `layer` is the watched view's, with the visibility the main thread saw last in `visibility`
// (window_lines.h's bits). an atomic load or two, no lock and no AppKit
bool window_lines_visibility(CAMetalLayer *layer, uint32_t *visibility);

// any thread: -[layer nextDrawable], its time in `waited_ns`, read around the Mac's call alone, and the nextDrawable
// line (docs/swtor/README.md § The nextDrawable line) when it took 50 ms or more
id<CAMetalDrawable> window_next_drawable(CAMetalLayer *layer, uint64_t *waited_ns);

#endif
