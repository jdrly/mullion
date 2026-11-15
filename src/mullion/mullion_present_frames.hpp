#pragma once

// MULLION_PRESENT_THREAD (docs/swtor/README.md § The presenter's thread): which of the presenter's frame textures a
// frame is drawn into, which frame the presenter's thread shows, and which frames are dropped. The queue draws a frame
// into a texture on its own command buffer (Draw); the buffer's completion makes it ready (Done); the thread takes the
// newest ready frame (Take), takes a drawable, copies the texture into it and gives the texture back once that copy has
// completed (Release). A texture the thread holds is never drawn into; frames are shown in order, an older one never
// after a newer one. The caller keeps all of it to one thread at a time (a mutex). Pure C++: tests/host/present_frames.cpp
// builds it natively.

#include <cstdint>
#include <optional>
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
  // the frame is still that texture's, now ready; a ready frame before it, which the thread didn't take, is dropped
  bool
  Done(unsigned index, uint64_t frame) {
    auto &texture = textures_[index];
    if (texture.state != State::Drawing || texture.frame != frame)
      return false;
    for (auto &other : textures_)
      if (other.state == State::Ready) {
        other.state = State::Free;
        dropped_++;
      }
    texture.state = State::Ready;
    return true;
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

} // namespace mullion::present_frames
