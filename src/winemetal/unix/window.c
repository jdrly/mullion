#include <time.h>
#include <unistd.h>
#include "window.h"
#include "window_lines.h"

// the main thread's alone: the view watched (retained, so a view its owner releases first is never read after), and
// what the lines said last
static NSView *watched;
static struct window_watch watch;

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
  struct timespec time;
  struct tm local;
  clock_gettime(CLOCK_REALTIME, &time);
  localtime_r(&time.tv_sec, &local);
  char at[16], line[160];
  snprintf(at, sizeof at, "%02d:%02d:%02d.%03ld", local.tm_hour, local.tm_min, local.tm_sec, time.tv_nsec / 1000000);
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
window_lines_watch(NSView *view) {
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
  [watched release];
  watched = [view retain];
  look("view");
}

void
window_lines_unwatch(NSView *view) {
  if (view != watched)
    return;
  [watched release];
  watched = nil;
}
