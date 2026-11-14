#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "window.h"
#include "window_lines.h"

// the main thread's alone: the view watched (retained, so a view its owner releases first is never read after), its
// layer, and what the lines said last
static NSView *watched;
static CAMetalLayer *watched_metal;
static struct window_watch watch;

// for the bridge's other threads: the watched view's layer (0 for none) and the visibility the main thread saw last,
// stored by it, never read through AppKit. the layer is stored after the visibility of its first look, and cleared
// before the view changes
static _Atomic(uintptr_t) watched_layer;
static _Atomic(uint32_t) watched_visibility;
// the present gate's (MULLION_PRESENT_GATE): the watched view's layer while the main thread last saw its window hidden,
// else 0. one load decides a present
static _Atomic(uintptr_t) hidden_layer;

// the local time, HH:MM:SS.mmm
static void
local_time(char at[16]) {
  struct timespec time;
  struct tm local;
  clock_gettime(CLOCK_REALTIME, &time);
  localtime_r(&time.tv_sec, &local);
  snprintf(at, 16, "%02d:%02d:%02d.%03ld", local.tm_hour, local.tm_min, local.tm_sec, time.tv_nsec / 1000000);
}

static void
look(const char *why) {
  if (!watched)
    return;
  NSWindow *window = watched.window;
  struct window_state now = {0, NSApp.isActive};
  // a view without a window counts as shown, as d9mt counts it
  if (!window)
    now.visibility = WINDOW_VISIBLE | WINDOW_ON_ACTIVE_SPACE;
  else {
    if (window.occlusionState & NSWindowOcclusionStateVisible)
      now.visibility |= WINDOW_VISIBLE;
    if (window.isOnActiveSpace)
      now.visibility |= WINDOW_ON_ACTIVE_SPACE;
    if (window.isMiniaturized)
      now.visibility |= WINDOW_MINIATURIZED;
  }
  window_lines_seen(now, why);
}

void
window_lines_seen(struct window_state now, const char *why) {
  atomic_store_explicit(&watched_visibility, now.visibility, memory_order_relaxed);
  atomic_store_explicit(&hidden_layer, window_shown(now.visibility) ? 0 : (uintptr_t)watched_metal,
                        memory_order_release);
  char at[16], line[160];
  local_time(at);
  int length = window_look(&watch, now, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), at, why, line, sizeof line);
  if (length)
    write(STDERR_FILENO, line, length);
}

// AppKit posts these on the main thread. no queue: the block runs as the notification is posted, so the line is
// written before the main thread goes on, which may be into a freeze
static void
observe(NSNotificationCenter *center, NSNotificationName name, const char *why) {
  [center addObserverForName:name
                      object:nil
                       queue:nil
                  usingBlock:^(NSNotification *notification) {
                    look(why);
                  }];
}

void
window_lines_watch(NSView *view, CAMetalLayer *layer) {
  if (view == watched)
    return;
  static bool observing;
  if (!observing) {
    observing = true;
    watch = window_watch_start();
    // any window's: look() reads the watched one's state, and writes nothing when it didn't change
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    observe(center, NSWindowDidChangeOcclusionStateNotification, "occlusion");
    observe(center, NSWindowDidMiniaturizeNotification, "miniaturize");
    observe(center, NSWindowDidDeminiaturizeNotification, "deminiaturize");
    observe(center, NSApplicationDidBecomeActiveNotification, "activate");
    observe(center, NSApplicationDidResignActiveNotification, "resign");
    observe(NSWorkspace.sharedWorkspace.notificationCenter, NSWorkspaceActiveSpaceDidChangeNotification, "space");
  }
  atomic_store_explicit(&watched_layer, 0, memory_order_release);
  atomic_store_explicit(&hidden_layer, 0, memory_order_release);
  [watched release];
  watched = [view retain];
  watched_metal = layer;
  look("view");
  atomic_store_explicit(&watched_layer, (uintptr_t)layer, memory_order_release);
}

void
window_lines_unwatch(NSView *view) {
  if (view != watched)
    return;
  atomic_store_explicit(&watched_layer, 0, memory_order_release);
  atomic_store_explicit(&hidden_layer, 0, memory_order_release);
  [watched release];
  watched = nil;
  watched_metal = nil;
}

bool
window_lines_visibility(CAMetalLayer *layer, uint32_t *visibility) {
  if (!layer || atomic_load_explicit(&watched_layer, memory_order_acquire) != (uintptr_t)layer)
    return false;
  *visibility = atomic_load_explicit(&watched_visibility, memory_order_relaxed);
  return true;
}

// the presenting threads' lines: a call this slow, or the gate closing or opening, is rare, and the lock is taken only
// then
static pthread_mutex_t drawable_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct window_limit drawable_limit;

static void
drawable_said(CAMetalLayer *layer, uint64_t waited_ns, bool got) {
  uint32_t visibility = DRAWABLE_VISIBILITY_UNKNOWN;
  window_lines_visibility(layer, &visibility);
  char at[16], line[160];
  local_time(at);
  pthread_mutex_lock(&drawable_mutex);
  int length = drawable_line(&drawable_limit, waited_ns, got, visibility, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), at,
                             line, sizeof line);
  pthread_mutex_unlock(&drawable_mutex);
  if (length)
    write(STDERR_FILENO, line, length);
}

static void
gate_said(CAMetalLayer *layer, bool closed, uint64_t skipped) {
  uint32_t visibility = DRAWABLE_VISIBILITY_UNKNOWN;
  window_lines_visibility(layer, &visibility);
  char at[16], line[160];
  local_time(at);
  pthread_mutex_lock(&drawable_mutex);
  int length = gate_line(&drawable_limit, closed, visibility, skipped, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), at,
                         line, sizeof line);
  pthread_mutex_unlock(&drawable_mutex);
  if (length)
    write(STDERR_FILENO, line, length);
}

id<CAMetalDrawable>
window_next_drawable(CAMetalLayer *layer, bool gate, uint64_t skipped, uint64_t *waited_ns, bool *gated) {
  // the gate: what AppKit last said, not a fresh look, which this thread mustn't take. at a switch away from the
  // window's Space the notification comes before the call that would block
  *gated = gate && layer && atomic_load_explicit(&hidden_layer, memory_order_acquire) == (uintptr_t)layer;
  if (*gated) {
    *waited_ns = 0;
    if (!skipped)
      gate_said(layer, true, 0);
    return nil;
  }
  if (skipped)
    gate_said(layer, false, skipped);
  // the Mac's call alone, so that a Space switch's block is its own
  uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  id<CAMetalDrawable> drawable = [layer nextDrawable];
  *waited_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
  if (drawable_blocked(*waited_ns))
    drawable_said(layer, *waited_ns, drawable != nil);
  return drawable;
}
