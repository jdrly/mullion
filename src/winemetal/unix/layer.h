#ifndef __WINEMETAL_LAYER_H
#define __WINEMETAL_LAYER_H

#import <QuartzCore/QuartzCore.h>
#include "../winemetal.h"

// what a present asks of its layer, set by the main thread when the layer doesn't hold it yet. with `wait`
// (MULLION_LAYER_WAIT=1) the caller waits for the main thread, as upstream does; without, it goes on and the main
// thread sets the changes later, in the order they were asked. the comparison runs on the calling thread
void layer_set_props(CAMetalLayer *layer, const struct WMTLayerProps *props, bool wait);

// `name` a colour space's name (kCGColorSpace...). false when that colour space can't be made
bool layer_set_colorspace(CAMetalLayer *layer, CFStringRef name, bool hdr, bool wait);

// MULLION_STATS' mainWaitMaxMs: the longest wait for the main thread, in ms, of the changes it applied since the last
// call, each counted as the mainThreadWaitMs lines count it, and of a change still waiting, so far. Takes it: the next
// call starts from 0. Either value of `wait`
uint64_t layer_take_main_wait_max_ms(void);

#endif
