// the bridge's layer setters (src/winemetal/unix/layer.c) with the main thread held: without waiting (MULLION_LAYER_WAIT=0)
// a present's call returns at once and the main thread sets the layer later, in the order asked; with it (=1) the call
// returns only once the main thread has set it. either way the bridge says when the main thread stops answering. a
// layer without a device, no window and no GPU
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#import <Foundation/Foundation.h>
#include "layer.h"

// stdout: the bridge's lines go to stderr, which the test reads back
#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      printf("failed: %s\n", #x);                                                                                      \
      exit(1);                                                                                                         \
    }                                                                                                                  \
  } while (0)

static uint64_t
now_ms(void) {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / NSEC_PER_MSEC;
}

// stderr's bytes from `from` on
static char *
logged(off_t from) {
  static char text[8192];
  ssize_t n = pread(STDERR_FILENO, text, sizeof(text) - 1, from);
  text[n > 0 ? n : 0] = 0;
  return text;
}

// runs the main queue until `done` or 10 s
static bool
run_main(bool (^done)(void)) {
  uint64_t until = now_ms() + 10000;
  while (!done()) {
    if (now_ms() > until)
      return false;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
  }
  return true;
}

static struct WMTLayerProps
props_of(CAMetalLayer *layer, double width, double height) {
  return (struct WMTLayerProps){
      .device = (obj_handle_t)layer.device,
      .contents_scale = layer.contentsScale,
      .drawable_width = width,
      .drawable_height = height,
      .opaque = layer.opaque,
      .display_sync_enabled = layer.displaySyncEnabled,
      .framebuffer_only = layer.framebufferOnly,
      .pixel_format = (enum WMTPixelFormat)layer.pixelFormat,
  };
}

static bool
shows(CAMetalLayer *layer, double width, double height, CFStringRef colorspace) {
  return CGSizeEqualToSize(layer.drawableSize, CGSizeMake(width, height)) && layer.colorspace &&
         CFEqual(CGColorSpaceGetName(layer.colorspace), colorspace);
}

// another thread sets the layer's size and colour space while the main thread is held 2 s without running its run
// loop; then the main thread runs it
static void
change(CAMetalLayer *layer, bool wait, double width, double height, CFStringRef colorspace) {
  off_t from = lseek(STDERR_FILENO, 0, SEEK_CUR);
  struct WMTLayerProps props = props_of(layer, width, height);
  // how long the calls took, -1 until they return
  static _Atomic int64_t took;
  atomic_store(&took, -1);
  dispatch_semaphore_t held = dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    uint64_t start = now_ms();
    layer_set_props(layer, &props, wait);
    CHECK(layer_set_colorspace(layer, colorspace, false, wait));
    atomic_store(&took, (int64_t)(now_ms() - start));
  });
  dispatch_semaphore_wait(held, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
  int64_t returned = atomic_load(&took);
  printf("wait=%d: the calls took %lld ms while the main thread was held\n", wait, (long long)returned);
  if (wait) {
    CHECK(returned == -1);
  } else {
    CHECK(returned >= 0 && returned < 100);
    CHECK(!shows(layer, width, height, colorspace));
  }
  // written once 1 s has passed
  CHECK(strstr(logged(from), " applied=no\n"));
  CHECK(run_main(^bool {
    return atomic_load(&took) >= 0 && shows(layer, width, height, colorspace);
  }));
  // the line of the change applied comes from the main thread, after it
  CHECK(run_main(^bool {
    return strstr(logged(from), " applied=yes\n") != NULL;
  }));
  printf("wait=%d: the layer is %gx%g once the main thread runs\n%s", wait, width, height, logged(from));
}

int
main(void) {
  // the bridge's lines, read back from here
  FILE *log = tmpfile();
  CHECK(log && dup2(fileno(log), STDERR_FILENO) == STDERR_FILENO);
  @autoreleasepool {
    CAMetalLayer *layer = [CAMetalLayer layer];
    change(layer, false, 640, 480, kCGColorSpaceSRGB);
    change(layer, true, 800, 600, kCGColorSpaceLinearSRGB);

    // asked 1024x768 and then 800x600 again before the main thread runs: the layer still holds 800x600, and still
    // has to end with it, not with the change that runs late
    struct WMTLayerProps larger = props_of(layer, 1024, 768), again = props_of(layer, 800, 600);
    dispatch_semaphore_t asked = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      layer_set_props(layer, &larger, false);
      layer_set_props(layer, &again, false);
      dispatch_semaphore_signal(asked);
    });
    dispatch_semaphore_wait(asked, DISPATCH_TIME_FOREVER);
    CHECK(CGSizeEqualToSize(layer.drawableSize, CGSizeMake(800, 600)));
    __block int turns = 0;
    CHECK(run_main(^bool {
      return ++turns > 20;
    }));
    CHECK(CGSizeEqualToSize(layer.drawableSize, CGSizeMake(800, 600)));
  }
  printf("passed\n");
  return 0;
}
