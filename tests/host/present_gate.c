// the present gate (src/winemetal/unix/window.c, MULLION_PRESENT_GATE): with the gate on, a layer that is the watched
// view's gets no -nextDrawable while the window was last seen hidden, off the active Space or in the Dock, and such a
// present returns at once; a call again once it is shown; with the gate off, a call every time; a layer that isn't the
// watched view's, a call every time (the gate fails open); and the gate's lines as it closes and opens. The watch is
// fed through window_lines_seen, the entry look() gives what it read from AppKit. no window, no GPU
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#import <AppKit/AppKit.h>
#include "window.h"

// stdout: the gate's lines go to stderr, which the test reads back
#define CHECK(x)                                                                                                       \
  do {                                                                                                                 \
    if (!(x)) {                                                                                                        \
      printf("failed: %s\n", #x);                                                                                      \
      exit(1);                                                                                                         \
    }                                                                                                                  \
  } while (0)

// a layer whose -nextDrawable counts its calls and takes 200 ms, as a drawable held at a Space switch
@interface CountingLayer : CAMetalLayer
@property(atomic) unsigned calls;
@end

@implementation CountingLayer
- (id<CAMetalDrawable>)nextDrawable {
  self.calls++;
  usleep(200000);
  return nil;
}
@end

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

// one present's call: whether it reached -nextDrawable, and its time
struct present {
  bool called, gated;
  uint64_t ms;
};

static struct present
present(CountingLayer *layer, bool gate, uint64_t *skipped) {
  unsigned calls = layer.calls;
  uint64_t began = now_ms(), waited_ns = 0;
  bool gated = false;
  window_next_drawable(layer, gate, *skipped, &waited_ns, &gated);
  *skipped = gated ? *skipped + 1 : 0;
  return (struct present){layer.calls != calls, gated, now_ms() - began};
}

static const uint32_t kShown = WINDOW_VISIBLE | WINDOW_ON_ACTIVE_SPACE;

int
main(void) {
  // the bridge's lines, from here on, into a file the test reads
  FILE *log = tmpfile();
  CHECK(log && dup2(fileno(log), STDERR_FILENO) == STDERR_FILENO);
  @autoreleasepool {
    CountingLayer *layer = [CountingLayer layer], *other = [CountingLayer layer];
    // a view without a window, which the watch counts as shown
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
    window_lines_watch(view, layer);
    uint64_t skipped = 0, other_skipped = 0;

    // shown: a call, which takes the layer's 200 ms
    struct present p = present(layer, true, &skipped);
    CHECK(p.called && !p.gated && p.ms >= 190);

    // a switch away: AppKit says the window can't be seen (occlusion), then that it is off the active Space
    off_t mark = lseek(STDERR_FILENO, 0, SEEK_END);
    window_lines_seen((struct window_state){WINDOW_ON_ACTIVE_SPACE, true}, "occlusion");
    for (unsigned i = 0; i < 3; i++) {
      p = present(layer, true, &skipped);
      CHECK(!p.called && p.gated && p.ms < 50);
    }
    window_lines_seen((struct window_state){0, true}, "space");
    p = present(layer, true, &skipped);
    CHECK(!p.called && p.gated && p.ms < 50);
    CHECK(skipped == 4);
    // one line as it closed, none for the presents after
    char *lines = logged(mark);
    CHECK(strstr(lines, "mullion: present gate closed visibility=0x2 skipped=0 at=") != NULL);
    CHECK(strstr(strstr(lines, "present gate closed") + 1, "present gate") == NULL);

    // another swap chain's layer, while the watched window is hidden: its call (the gate fails open)
    p = present(other, true, &other_skipped);
    CHECK(p.called && !p.gated && p.ms >= 190);

    // MULLION_PRESENT_GATE=0 while it is hidden: a call every time
    uint64_t off_skipped = 0;
    for (unsigned i = 0; i < 2; i++) {
      p = present(layer, false, &off_skipped);
      CHECK(p.called && !p.gated && p.ms >= 190);
    }

    // in the Dock, and partly covered on the active Space but in the Dock: gated
    window_lines_seen((struct window_state){WINDOW_MINIATURIZED, true}, "miniaturize");
    p = present(layer, true, &skipped);
    CHECK(!p.called && p.gated);
    window_lines_seen((struct window_state){kShown | WINDOW_MINIATURIZED, true}, "miniaturize");
    p = present(layer, true, &skipped);
    CHECK(!p.called && p.gated);

    // back: a call again, and one line as it opens, with the presents skipped
    mark = lseek(STDERR_FILENO, 0, SEEK_END);
    window_lines_seen((struct window_state){kShown, true}, "deminiaturize");
    p = present(layer, true, &skipped);
    CHECK(p.called && !p.gated && p.ms >= 190);
    CHECK(skipped == 0);
    lines = logged(mark);
    CHECK(strstr(lines, "mullion: present gate open visibility=0x3 skipped=6 at=") != NULL);
    p = present(layer, true, &skipped);
    CHECK(p.called && !p.gated);
    CHECK(strstr(logged(mark), "present gate closed") == NULL);

    // a view no longer watched: its layer's calls go on whatever its window was last seen as
    window_lines_seen((struct window_state){0, true}, "space");
    window_lines_unwatch(view);
    p = present(layer, true, &skipped);
    CHECK(p.called && !p.gated);
    [view release];
  }
  printf("passed\n");
  return 0;
}
