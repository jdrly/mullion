// contract: the presenter's frame textures (src/mullion/mullion_present_frames.hpp, MULLION_PRESENT_THREAD): frames
// are drawn, made ready and shown in order; while the presenter's thread holds a texture (it waits for a drawable),
// the queue keeps drawing into others and only the newest completed frame waits for the thread, the rest dropped and
// counted; a texture the thread holds is never drawn into, at the cap either; a frame overwritten before its buffer
// completed is never shown; and a frame without a drawable counts as dropped
#include "../../src/mullion/mullion_present_frames.hpp"
#include <cstdio>
#include <string>

using namespace mullion::present_frames;

static unsigned failures = 0;

static void
check(bool ok, const std::string &what) {
  if (!ok)
    failures++, std::printf("failed: %s\n", what.c_str());
}

int
main() {
  // a steady stream: one texture drawn, completed, shown and given back each frame
  {
    Book book;
    bool grew = false;
    for (uint64_t frame = 1; frame <= 5; frame++) {
      unsigned index = book.Draw(frame, grew);
      check(index == 0 && grew == (frame == 1), "a steady stream reuses one texture");
      check(book.Done(index, frame), "its frame is ready once drawn");
      auto taken = book.Take();
      check(taken && taken->first == index && taken->second == frame, "the thread takes it");
      book.Release(index, true);
    }
    check(book.TakeDropped() == 0, "a steady stream drops nothing");
  }

  // the GPU two frames behind: three textures, each frame shown in order
  {
    Book book;
    bool grew = false;
    unsigned a = book.Draw(1, grew), b = book.Draw(2, grew), c = book.Draw(3, grew);
    check(a != b && b != c && a != c && book.textures().size() == 3, "frames in flight get textures of their own");
    check(book.Done(a, 1), "frame 1 ready");
    auto taken = book.Take();
    check(taken && taken->second == 1, "the thread shows frame 1 first");
    book.Release(a, true);
    check(book.Done(b, 2) && book.Take()->second == 2, "then frame 2");
    book.Release(b, true);
    check(book.Done(c, 3) && book.Take()->second == 3, "then frame 3");
    book.Release(c, true);
    check(book.TakeDropped() == 0, "and drops nothing");
  }

  // the thread waits for a drawable (a Space switch): it holds frame 1 while the queue draws 140 more
  {
    Book book;
    bool grew = false;
    unsigned held = book.Draw(1, grew);
    book.Done(held, 1);
    auto taken = book.Take();
    check(taken && taken->second == 1, "the thread holds frame 1");
    for (uint64_t frame = 2; frame <= 141; frame++) {
      unsigned index = book.Draw(frame, grew);
      check(index != held, "a texture the thread holds is never drawn into");
      check(book.Done(index, frame), "each frame completes");
    }
    check(book.textures().size() <= 3, "a stalled thread needs three textures, not one per frame");
    check(book.TakeDropped() == 139, "the frames the thread couldn't show are dropped and counted (139)");
    book.Release(held, true);
    auto next = book.Take();
    check(next && next->second == 141, "once the drawable comes, the newest frame is shown, never an older one");
  }

  // the cap: frames drawn that don't complete (the GPU behind, or a buffer cancelled) take textures up to 16, then the
  // oldest not held is overwritten; its completion shows nothing
  {
    Book book;
    bool grew = false;
    unsigned held = book.Draw(1, grew);
    book.Done(held, 1);
    book.Take();
    unsigned first = book.Draw(2, grew);
    for (uint64_t frame = 3; frame <= kMaxTextures; frame++)
      book.Draw(frame, grew);
    check(book.textures().size() == kMaxTextures && book.TakeDropped() == 0, "16 textures, nothing dropped yet");
    unsigned over = book.Draw(kMaxTextures + 1, grew);
    check(!grew && over == first, "at the cap the oldest frame not shown is overwritten");
    check(over != held, "at the cap a texture the thread holds is never drawn into");
    check(book.TakeDropped() == 1, "the frame overwritten is dropped");
    check(!book.Done(first, 2), "the overwritten frame's completion shows nothing");
    check(book.Done(over, kMaxTextures + 1), "the frame drawn over it completes");
  }

  // a frame without a drawable (its 1 s timeout) is dropped, and its texture is free again
  {
    Book book;
    bool grew = false;
    unsigned index = book.Draw(1, grew);
    book.Done(index, 1);
    book.Take();
    book.Release(index, false);
    check(book.TakeDropped() == 1, "a frame without a drawable counts as dropped");
    check(book.Draw(2, grew) == index && !grew, "and its texture is drawn into again");
  }

  if (failures)
    std::printf("%u failed\n", failures);
  else
    std::printf("passed\n");
  return failures ? 1 : 0;
}
