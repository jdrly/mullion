#pragma once

// MULLION_PRESENT_THREAD (docs/swtor/README.md § The presenter's thread): which of the presenter's frame textures a
// frame is drawn into, which frame the presenter's thread shows, and which frames are dropped. The queue draws a frame
// into a texture on its own command buffer (Draw); the buffer's completion makes it ready (Done); the thread takes the
// newest ready frame (Take), takes a drawable, copies the texture into it and gives the texture back once that copy has
// completed (Release). A texture the thread holds is never drawn into; frames are shown in order, an older one never
// after a newer one. Book is for one thread at a time; Mailbox puts it under a lock and is what the presenter and its
// thread share. Pure C++: tests/host/present_frames.cpp builds it natively.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mullion::present_frames {

// DXGI_MAX_SWAP_CHAIN_BUFFERS: past this many textures, a frame overwrites the oldest one not shown yet
constexpr unsigned kMaxTextures = 16;

enum class State : uint8_t {
  Free,    // nothing reads or writes it
  Drawing, // a queue's command buffer draws a frame into it
  Ready,   // that buffer completed: the thread may take it
  Held,    // the thread copies it into a drawable
};

struct Texture {
  State state = State::Free;
  // the frame drawn into it last, numbered as Draw was told: increasing
  uint64_t frame = 0;
};

class Book {
public:
  // the queue's thread: the texture frame `frame` is drawn into; `grew` is set when it is a new one, which the caller
  // makes. One that is drawing or ready is overwritten only at the cap, the oldest first, and its frame is dropped
  unsigned
  Draw(uint64_t frame, bool &grew) {
    grew = false;
    unsigned index = textures_.size();
    for (unsigned i = 0; i < textures_.size(); i++)
      if (textures_[i].state == State::Free) {
        index = i;
        break;
      }
    if (index == textures_.size()) {
      if (textures_.size() < kMaxTextures) {
        textures_.emplace_back();
        grew = true;
      } else {
        for (unsigned i = 0; i < textures_.size(); i++)
          if (textures_[i].state != State::Held && (index == textures_.size() || textures_[i].frame < textures_[index].frame))
            index = i;
        dropped_++;
      }
    }
    textures_[index] = {State::Drawing, frame};
    return index;
  }

  // the completion thread: the command buffer that drew `frame` into `index` completed, in the queue's order. true when
  // the frame is still that texture's, now ready; a ready frame before it, which the thread didn't take, is dropped,
  // and so is one still drawing before it, whose buffer will never complete (a cancelled one)
  bool
  Done(unsigned index, uint64_t frame) {
    auto &texture = textures_[index];
    if (texture.state != State::Drawing || texture.frame != frame)
      return false;
    for (auto &other : textures_)
      if (other.state == State::Ready || (other.state == State::Drawing && other.frame < frame)) {
        other.state = State::Free;
        dropped_++;
      }
    texture.state = State::Ready;
    return true;
  }

  // the frames drawn and neither shown nor dropped yet
  unsigned
  Outstanding() const {
    unsigned outstanding = 0;
    for (auto &texture : textures_)
      outstanding += texture.state != State::Free;
    return outstanding;
  }

  bool
  HasReady() const {
    for (auto &texture : textures_)
      if (texture.state == State::Ready)
        return true;
    return false;
  }

  // the presenter's thread: the ready frame's texture and frame, now held, or none
  std::optional<std::pair<unsigned, uint64_t>>
  Take() {
    for (unsigned i = 0; i < textures_.size(); i++)
      if (textures_[i].state == State::Ready) {
        textures_[i].state = State::Held;
        return std::pair{i, textures_[i].frame};
      }
    return std::nullopt;
  }

  // the presenter's thread: done with `index`, its copy completed or not made. `shown` false counts its frame as
  // dropped (no drawable)
  void
  Release(unsigned index, bool shown) {
    textures_[index].state = State::Free;
    if (!shown)
      dropped_++;
  }

  // the frames dropped since the last call
  uint64_t
  TakeDropped() {
    return std::exchange(dropped_, 0);
  }

  const std::vector<Texture> &
  textures() const {
    return textures_;
  }

private:
  std::vector<Texture> textures_;
  uint64_t dropped_ = 0;
};

// The book under a lock, with what each texture carries (`Payload`), the presenter's thread's wait, and its stop. The
// presenter and its thread share it (std::shared_ptr), and the thread keeps it after the presenter is gone: a
// presenter's release only asks the thread to stop, and never waits for it, which may be waiting up to the drawable
// timeout (a presenter's last release can come on the queue's completion thread, ahead of the fences it retires)
template <typename Mutex, typename CondVar, typename Payload> class Mailbox {
public:
  struct Taken {
    unsigned index;
    uint64_t frame;
    Payload payload;
  };

  // the queue's thread: Book::Draw, then `prepare` with the texture's payload, under the lock
  template <typename Prepare>
  unsigned
  Draw(uint64_t frame, Prepare &&prepare) {
    std::lock_guard<Mutex> lock(mutex_);
    bool grew;
    unsigned index = book_.Draw(frame, grew);
    if (grew)
      payloads_.emplace_back();
    prepare(payloads_[index]);
    return index;
  }

  // the completion thread: Book::Done, and `ready` with the frame's payload when it is ready for the thread
  template <typename Ready>
  void
  Done(unsigned index, uint64_t frame, Ready &&ready) {
    {
      std::lock_guard<Mutex> lock(mutex_);
      if (!book_.Done(index, frame))
        return;
      ready(payloads_[index]);
    }
    changed_.notify_all();
  }

  // the presenter's thread: the frame to show next, now held, or none once stopped
  std::optional<Taken>
  Wait() {
    std::unique_lock<Mutex> lock(mutex_);
    changed_.wait(lock, [&] { return stopping_ || book_.HasReady(); });
    if (stopping_)
      return std::nullopt;
    auto taken = book_.Take();
    return Taken{taken->first, taken->second, payloads_[taken->first]};
  }

  // the presenter's thread: Book::Release, and the frames dropped since the last call
  uint64_t
  Release(unsigned index, bool shown) {
    uint64_t dropped;
    {
      std::lock_guard<Mutex> lock(mutex_);
      book_.Release(index, shown);
      dropped = book_.TakeDropped();
    }
    changed_.notify_all();
    return dropped;
  }

  // the game's thread, before a Present: waits while `limit` frames or more are drawn and neither shown nor dropped,
  // at most `timeout`, so that the game goes no faster than the drawables come (the commit thread's nextDrawable paced
  // it before) and no further ahead than `limit` frames. false when it gave up: the thread waits for a drawable that
  // doesn't come (a Space switch), and the game goes on, its frames dropped
  template <typename Rep, typename Period>
  bool
  Throttle(unsigned limit, std::chrono::duration<Rep, Period> timeout) {
    auto until = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<Mutex> lock(mutex_);
    while (!stopping_ && book_.Outstanding() >= limit) {
      auto now = std::chrono::steady_clock::now();
      if (now >= until)
        return false;
      changed_.wait_for(lock, until - now);
    }
    return true;
  }

  // the presenter, as it goes: the thread ends at its next Wait. Returns at once
  void
  Stop() {
    {
      std::lock_guard<Mutex> lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_all();
  }

private:
  Mutex mutex_;
  CondVar changed_;
  Book book_;
  std::vector<Payload> payloads_;
  bool stopping_ = false;
};

// the dropped line: `dropped` frames since `since_ms` before it, the longest wait for a drawable among them
inline std::string
DroppedLine(uint64_t dropped, uint64_t since_ms, uint64_t drawable_ms, const char *at) {
  char text[160];
  std::snprintf(text, sizeof(text), "Mullion: present dropped=%llu since=%llu drawableMs=%llu at=%s",
                (unsigned long long)dropped, (unsigned long long)since_ms, (unsigned long long)drawable_ms, at);
  return text;
}

} // namespace mullion::present_frames
