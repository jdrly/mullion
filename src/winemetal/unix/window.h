#ifndef __WINEMETAL_WINDOW_H
#define __WINEMETAL_WINDOW_H

#import <AppKit/AppKit.h>

// the window lines (docs/swtor/README.md § The window lines): a line on the process's stderr at each change of the
// visibility of the watched view's window, or of the app's activation. both on the main thread

// watches `view`'s window from now on, in place of the one before: the last view a swap chain made
void window_lines_watch(NSView *view);

// stops watching `view` when it is the one watched; before the view is released
void window_lines_unwatch(NSView *view);

#endif
