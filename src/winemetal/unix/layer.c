#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#import <Foundation/Foundation.h>
#include "layer.h"

// changes sent to the main thread, and those it has applied. the main queue runs them in the order they were sent
static _Atomic uint64_t sent, applied;
// when the main thread last applied a change, or was sent one with none waiting
static _Atomic uint64_t since_ns;
// `applied` when the main thread last applied a change, shifted left by 2, and how many `applied=no` lines the wait
// after it has written: a line of a wait that has just ended can't be written after that change's `applied=yes`
static _Atomic uint64_t said;
static atomic_bool watching;
static const uint64_t marks_ms[] = {1000, 5000, 30000};

static uint64_t
now_ns(void) {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// ms from `since` to `now`, 0 when another thread's `since` came later
static uint64_t
since_ms(uint64_t since, uint64_t now) {
  return now > since ? (now - since) / NSEC_PER_MSEC : 0;
}

static void watch(void);

static void
watch_later(void) {
  static dispatch_queue_t queue;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    queue = dispatch_queue_create("mullion-layer-watch", DISPATCH_QUEUE_SERIAL);
  });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), queue, ^{
    watch();
  });
}

// every 250 ms while a change waits for the main thread: a line once it has applied none for 1 s, 5 s and 30 s. on a
// queue of its own, so that the line comes whether anything still presents or not
static void
watch(void) {
  if (atomic_load(&applied) == atomic_load(&sent)) {
    atomic_store(&watching, false);
    // a change sent meanwhile found the watch still on, and left it to this one
    if (atomic_load(&applied) == atomic_load(&sent) || atomic_exchange(&watching, true))
      return;
  }
  uint64_t done = atomic_load(&applied);
  uint64_t ms = since_ms(atomic_load(&since_ns), now_ns());
  uint64_t was = atomic_load(&said), due = was & 3;
  while (due < 3 && ms >= marks_ms[due])
    due++;
  // not while the main thread is between applying a change and saying so
  if (was >> 2 == done && due > (was & 3) && atomic_compare_exchange_strong(&said, &was, done << 2 | due))
    fprintf(stderr, "mullion: layer mainThreadWaitMs=%llu applied=no\n", (unsigned long long)ms);
  watch_later();
}

// on the main thread, after a change
static void
applied_one(void) {
  uint64_t now = now_ns();
  uint64_t ms = since_ms(atomic_exchange(&since_ns, now), now);
  uint64_t done = atomic_fetch_add(&applied, 1) + 1;
  if (atomic_exchange(&said, done << 2) & 3)
    fprintf(stderr, "mullion: layer mainThreadWaitMs=%llu applied=yes\n", (unsigned long long)ms);
}

// runs `set` on the main thread, after what was sent to it before, and waits for it with `wait`. its number among the
// changes sent, 0 when the caller is the main thread and ran it
static uint64_t
to_main(bool wait, dispatch_block_t set) {
  if ([NSThread isMainThread]) {
    set();
    return 0;
  }
  if (atomic_load(&applied) == atomic_load(&sent))
    atomic_store(&since_ns, now_ns());
  uint64_t number = atomic_fetch_add(&sent, 1) + 1;
  if (!atomic_exchange(&watching, true))
    watch_later();
  // the queue's copy of this block copies `set`, and that copy retains the layer it captured (an Objective-C object)
  dispatch_block_t block = ^{
    set();
    applied_one();
  };
  if (wait)
    dispatch_sync(dispatch_get_main_queue(), block);
  else
    dispatch_async(dispatch_get_main_queue(), block);
  return number;
}

// the newest change of each kind sent without waiting, and its number: it waits while that is above `applied`, and
// the layer ends with it. `name` is a constant (kCGColorSpace...). a present whose layer holds what it asks reads
// neither and takes no lock, unless a change to any layer is still waiting: the counters are the process's
static pthread_mutex_t asked_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  CAMetalLayer *layer;
  struct WMTLayerProps props;
  uint64_t number;
} asked_props;
static struct {
  CAMetalLayer *layer;
  CFStringRef name;
  bool hdr;
  uint64_t number;
} asked_colorspace;

static bool
holds_props(CAMetalLayer *layer, const struct WMTLayerProps *props) {
  return layer.device == (id<MTLDevice>)props->device && layer.opaque == props->opaque &&
         layer.framebufferOnly == props->framebuffer_only && layer.contentsScale == props->contents_scale &&
         layer.displaySyncEnabled == props->display_sync_enabled &&
         CGSizeEqualToSize(layer.drawableSize, CGSizeMake(props->drawable_width, props->drawable_height)) &&
         layer.pixelFormat == (MTLPixelFormat)ORIGINAL_FORMAT(props->pixel_format);
}

static bool
same_props(const struct WMTLayerProps *a, const struct WMTLayerProps *b) {
  return a->device == b->device && a->opaque == b->opaque && a->framebuffer_only == b->framebuffer_only &&
         a->contents_scale == b->contents_scale && a->display_sync_enabled == b->display_sync_enabled &&
         a->drawable_width == b->drawable_width && a->drawable_height == b->drawable_height &&
         a->pixel_format == b->pixel_format;
}

void
layer_set_props(CAMetalLayer *layer, const struct WMTLayerProps *asked, bool wait) {
  // a copy: without `wait` the main thread sets it after the caller has returned
  struct WMTLayerProps props = *asked;
  // a present asks each frame, and only a change goes to the main thread. without `wait`, while changes are on their
  // way the layer isn't what it will hold. the counters are read before the layer: a change the main thread applies
  // after them has to be one sent after them
  bool idle = atomic_load(&applied) == atomic_load(&sent);
  if ((wait || idle) && holds_props(layer, &props))
    return;
  dispatch_block_t set = ^{
    layer.device = (id<MTLDevice>)props.device;
    layer.opaque = props.opaque;
    layer.framebufferOnly = props.framebuffer_only;
    layer.contentsScale = props.contents_scale;
    layer.displaySyncEnabled = props.display_sync_enabled;
    layer.drawableSize = CGSizeMake(props.drawable_width, props.drawable_height);
    layer.pixelFormat = (MTLPixelFormat)ORIGINAL_FORMAT(props.pixel_format);
  };
  if (wait) {
    to_main(true, set);
    return;
  }
  pthread_mutex_lock(&asked_lock);
  // the newest change still waiting is what the layer will hold when it is this layer's. another layer's may have one
  // of this layer's behind it
  bool waiting = asked_props.number > atomic_load(&applied);
  if (waiting ? asked_props.layer != layer || !same_props(&asked_props.props, &props) : !holds_props(layer, &props)) {
    asked_props.layer = layer;
    asked_props.props = props;
    asked_props.number = to_main(false, set);
  }
  pthread_mutex_unlock(&asked_lock);
}

static bool
holds_colorspace(CAMetalLayer *layer, CFStringRef name, bool hdr) {
  CFStringRef current = layer.colorspace ? CGColorSpaceGetName(layer.colorspace) : NULL;
  // compared as bools: the layer may hold a 4, from WMT_COLORSPACE_IS_HDR's `x & 0b100`
  return current && CFEqual(current, name) && (bool)layer.wantsExtendedDynamicRangeContent == hdr;
}

bool
layer_set_colorspace(CAMetalLayer *layer, CFStringRef name, bool hdr, bool wait) {
  // as layer_set_props
  bool idle = atomic_load(&applied) == atomic_load(&sent);
  if ((wait || idle) && holds_colorspace(layer, name, hdr))
    return true;
  if (!wait) {
    pthread_mutex_lock(&asked_lock);
    bool waiting = asked_colorspace.number > atomic_load(&applied);
    bool same = asked_colorspace.layer == layer && CFEqual(asked_colorspace.name, name) && asked_colorspace.hdr == hdr;
    if (waiting ? same : holds_colorspace(layer, name, hdr)) {
      pthread_mutex_unlock(&asked_lock);
      return true;
    }
  }
  CGColorSpaceRef ref = CGColorSpaceCreateWithName(name);
  // the block owns the reference and releases it
  if (ref) {
    uint64_t number = to_main(wait, ^{
      layer.colorspace = ref;
      layer.wantsExtendedDynamicRangeContent = hdr;
      CGColorSpaceRelease(ref);
    });
    if (!wait) {
      asked_colorspace.layer = layer;
      asked_colorspace.name = name;
      asked_colorspace.hdr = hdr;
      asked_colorspace.number = number;
    }
  }
  if (!wait)
    pthread_mutex_unlock(&asked_lock);
  return ref != NULL;
}
