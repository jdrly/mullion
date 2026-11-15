// contract: the presenter's frame textures (src/mullion/mullion_present_frames.hpp, MULLION_PRESENT_THREAD): frames
// are drawn, made ready and shown in order; while the presenter's thread holds a texture (it waits for a drawable),
// the queue keeps drawing into others and only the newest completed frame waits for the thread, the rest dropped and
// counted; a texture the thread holds is never drawn into, at the cap either; a frame overwritten before its buffer
// completed is never shown; a frame without a drawable counts as dropped; the presenter stops its thread without
// waiting for it while the thread waits for a drawable (its last release can come on the queue's completion thread),
// and the thread ends after that wait; and the dropped line's format
#include "../../src/mullion/mullion_present_frames.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

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

  // the presenter goes while its thread waits 500 ms for a drawable: Stop returns at once, the thread keeps the mailbox
  // and ends after its wait, at its next Wait
  {
    using Box = Mailbox<std::mutex, std::condition_variable, int>;
    auto box = std::make_shared<Box>();
    std::atomic<bool> waiting{false}, ended{false};
    std::thread thread([box, &waiting, &ended] {
      while (auto taken = box->Wait()) {
        waiting = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        box->Release(taken->index, true);
      }
      ended = true;
    });
    unsigned index = box->Draw(1, [](int &payload) { payload = 7; });
    box->Done(index, 1, [](int &) {});
    while (!waiting)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto began = std::chrono::steady_clock::now();
    box->Stop();
    box.reset();
    auto took = std::chrono::steady_clock::now() - began;
    check(took < std::chrono::milliseconds(100),
          "stopping doesn't wait for a thread that waits for a drawable (took " +
              std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms)");
    check(!ended, "the thread is still in its wait as the presenter goes");
    thread.join();
    check(ended, "the thread ends after its wait");
  }

  // the game's pace: a Present waits while the thread is two frames behind, until it shows one, and gives up after its
  // timeout while the thread waits for a drawable that doesn't come
  {
    using Box = Mailbox<std::mutex, std::condition_variable, int>;
    Box box;
    auto ms = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
    unsigned a = box.Draw(1, [](int &) {});
    auto began = std::chrono::steady_clock::now();
    check(box.Throttle(2, std::chrono::milliseconds(100)) && ms(std::chrono::steady_clock::now() - began) < 50,
          "one frame ahead: a Present goes on at once");
    box.Done(a, 1, [](int &) {});
    auto held = box.Wait();
    unsigned b = box.Draw(2, [](int &) {});
    std::thread shows([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      box.Release(held->index, true);
    });
    began = std::chrono::steady_clock::now();
    bool kept = box.Throttle(2, std::chrono::milliseconds(500));
    auto waited = ms(std::chrono::steady_clock::now() - began);
    shows.join();
    check(kept && waited >= 20 && waited < 400,
          "two frames ahead: a Present waits for the thread to show one (waited " + std::to_string(waited) + " ms)");
    box.Done(b, 2, [](int &) {});
    auto blocked = box.Wait();
    box.Draw(3, [](int &) {});
    began = std::chrono::steady_clock::now();
    kept = box.Throttle(2, std::chrono::milliseconds(100));
    waited = ms(std::chrono::steady_clock::now() - began);
    check(!kept && waited >= 90 && waited < 400,
          "a thread waiting for a drawable holds a Present up for 100 ms at most (waited " + std::to_string(waited) + " ms)");
    box.Release(blocked->index, true);
  }

  // a frame whose buffer never completes (cancelled) is dropped once a later one completes: it holds no Present up
  {
    Book book;
    bool grew = false;
    unsigned lost = book.Draw(1, grew), next = book.Draw(2, grew);
    check(book.Done(next, 2) && book.textures()[lost].state == State::Free && book.Outstanding() == 1,
          "a frame drawing before one that completed is dropped");
    check(book.TakeDropped() == 1, "and counted");
  }

  check(DroppedLine(139, 1201, 1193, "02:42:05.905") ==
            "Mullion: present dropped=139 since=1201 drawableMs=1193 at=02:42:05.905",
        "the dropped line");

  if (failures)
    std::printf("%u failed\n", failures);
  else
    std::printf("passed\n");
  return failures ? 1 : 0;
}
