#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "window.h"
#include "window_lines.h"

// the main thread's alone: the view watched (retained, so a view its owner releases first is never read after), and
// what the lines said last
static NSView *watched;
static struct window_watch watch;

// for the bridge's other threads: the watched view's layer (0 for none) and the visibility the main thread saw last,
// stored by it, never read through AppKit. the layer is stored after the visibility of its first look, and cleared
// before the view changes
static _Atomic(uintptr_t) watched_layer;
static _Atomic(uint32_t) watched_visibility;

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
  atomic_store_explicit(&watched_visibility, now.visibility, memory_order_relaxed);
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
  [watched release];
  watched = [view retain];
  look("view");
  atomic_store_explicit(&watched_layer, (uintptr_t)layer, memory_order_release);
}

void
window_lines_unwatch(NSView *view) {
  if (view != watched)
    return;
  atomic_store_explicit(&watched_layer, 0, memory_order_release);
  [watched release];
  watched = nil;
}

bool
window_lines_visibility(CAMetalLayer *layer, uint32_t *visibility) {
  if (!layer || atomic_load_explicit(&watched_layer, memory_order_acquire) != (uintptr_t)layer)
    return false;
  *visibility = atomic_load_explicit(&watched_visibility, memory_order_relaxed);
  return true;
}

void
window_lines_drawable(CAMetalLayer *layer, uint64_t waited_ns, bool got) {
  if (!drawable_blocked(waited_ns))
    return;
  // the presenting threads': a call this slow is rare, and the lock is taken only then
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  static struct window_limit limit;
  uint32_t visibility = DRAWABLE_VISIBILITY_UNKNOWN;
  window_lines_visibility(layer, &visibility);
  char at[16], line[160];
  local_time(at);
  pthread_mutex_lock(&mutex);
  int length = drawable_line(&limit, waited_ns, got, visibility, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), at, line,
                             sizeof line);
  pthread_mutex_unlock(&mutex);
  if (length)
    write(STDERR_FILENO, line, length);
}
