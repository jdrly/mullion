#pragma once
#include <cstring>
#include "Metal.hpp"
#include "dxgi.h"
#include "mullion_command.hpp"
#include "rc/util_rc.hpp"
#include "thread.hpp"
#include "winemetal.h"
#include "wsi_platform.hpp"
#include "mullion_present_frames.hpp"
#include <atomic>
#include <deque>
#include <memory>

namespace mullion {

// a copy the runtime makes from the previous frame onto the one a flip model swap chain presents: its destination,
// and its source's offset from it
struct PresentCopy {
  RECT to;
  POINT from;
};

// how a back buffer is put on its target: the part of it that is presented, from its origin, as a fraction of each
// side; the target's size over the picture's, 1 for a picture stretched over the target; the back buffer's place for
// a place in the picture (rows of a matrix, and what each adds), which turns a picture that the back buffer holds
// rotated; and the color around a picture smaller than the target. Metal aligns a vector to its size
struct MullionPresentation {
  alignas(2 * sizeof(float)) float source[2] = {1, 1};
  float scale[2] = {1, 1};
  alignas(4 * sizeof(float)) float turn[4] = {1, 0, 0, 1};
  float shift[2] = {0, 0};
  alignas(4 * sizeof(float)) float background[4] = {0, 0, 0, 1};

  // all of a back buffer as it is, over all of the target
  bool
  plain() const {
    static constexpr float whole[] = {1, 1, 1, 1, 1, 0, 0, 1, 0, 0};
    return !memcmp(source, whole, sizeof(source)) && !memcmp(scale, whole + 2, sizeof(scale)) &&
           !memcmp(turn, whole + 4, sizeof(turn)) && !memcmp(shift, whole + 8, sizeof(shift));
  }
};

struct MullionPresentMetadata {
  float edr_scale;
  float max_content_luminance;
  float max_display_luminance;
  MullionPresentation presentation;
};

constexpr uint32_t MULLION_GAMMA_CP_COUNT = 1024;

struct MullionGammaRamp {
  float red[MULLION_GAMMA_CP_COUNT];
  float green[MULLION_GAMMA_CP_COUNT];
  float blue[MULLION_GAMMA_CP_COUNT];
  uint64_t version;
};

class Presenter : public RcObject {
public:
  // without a layer (a Wine whose winemac.drv hides the Metal view exports), frames reach `window` through GDI
  Presenter(
      WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count,
      HWND window
  );

  // asks the presenter's thread (MULLION_PRESENT_THREAD) to stop, and doesn't wait for it: the thread keeps what it
  // shares with the presenter until it ends
  ~Presenter();

  bool changeLayerProperties(
      DXGI_FORMAT format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
  );

  bool changeLayerColorSpace(WMTColorSpace colorspace);

  void changeHDRMetadata(const WMTHDRMetadata *metadata);

  void changeGammaRamp(const MullionGammaRamp *gamma_ramp);

  class PresentState {
  public:
    MullionPresentMetadata metadata;
    WMTLayerProps layer_props;
    WMTColorSpace colorspace;
    WMT::Reference<WMT::Texture> gamma_lut;
    WMT::Reference<WMT::RenderPipelineState> blit, scale;
  };

  PresentState synchronizeLayerProperties();

  // where a MetalFX scaler upscales `input` to: the layer's size on screen, in pixels, so a frame is shown at the
  // display's (in fullscreen) or the window's resolution. false while the layer has no size, or one smaller than the
  // input (the caller upscales by its factor then)
  bool upscaleOutputSize(uint32_t input_width, uint32_t input_height, uint32_t &width, uint32_t &height);

  // how later frames are put on the target (the swap chain's settings)
  void
  changePresentation(const MullionPresentation &presentation) {
    presentation_ = presentation;
  }

  // MULLION_PRESENT_THREAD (docs/swtor/README.md § The presenter's thread): a frame encoded with `deferred` is drawn
  // into a texture of the presenter's own, and its drawable is taken on the presenter's thread once Show hands it over
  struct Deferred {
    unsigned index = 0;
    uint64_t frame = 0;
    // kept until the command buffer that draws into it completes (ADR 0002)
    WMT::Reference<WMT::Texture> texture;
  };

  // whether the queue should pass `deferred`: the switch is on and the window has a layer
  bool
  threaded() const {
    return thread_on_ && layer_;
  }

  // on a Metal 3 or a Metal 4 command buffer. `drawable_ns`, unless null: the time the bridge's nextDrawable took, 0
  // without a call (HITCH's drawableMs); with `deferred`, the presenter's thread's time in it since the last present.
  // `deferred`, when threaded(): no drawable is taken here, nothing is to be presented on `cmdbuf`, and the frame is to
  // go to Show once `cmdbuf` completes
  template <
      typename CommandBuffer,
      typename Encoder =
          decltype(std::declval<CommandBuffer>().renderCommandEncoder(std::declval<WMTRenderPassInfo &>()))>
  WMT::MetalDrawable encodeCommands(
      CommandBuffer cmdbuf, WMT::Texture backbuffer, const PresentState &state,
      std::type_identity_t<std::function<void(Encoder)>> &&wait_fences,
      std::type_identity_t<std::function<void(Encoder)>> &&update_fences, uint64_t seq_id = 0,
      unsigned *encoder_count = nullptr, uint64_t *drawable_ns = nullptr, Deferred *deferred = nullptr
  );

  // the completion thread, once the command buffer of a frame encoded with `deferred` completed: the presenter's
  // thread shows it, unless a newer frame completes first; `after` as presentDrawableAfterMinimumDuration's
  void show(const Deferred &deferred, double after);

  // draws the oldest frame encoded without a layer, at least `after` seconds after the previous one (as
  // presentDrawableAfterMinimumDuration does); call it once that frame's command buffer has completed
  void drawToWindow(double after);

  // the frames shown (IDXGISwapChain::GetFrameStatistics), counting refreshes at `refresh_rate` since the presenter
  // was made
  HRESULT frameStatistics(DXGI_FRAME_STATISTICS *stats, double refresh_rate);

private:
  void buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable);

  WMT::Device device_;
  WMT::MetalLayer layer_;
  InternalCommandLibrary &lib_;
  WMTLayerProps layer_props_;
  // MULLION_LAYER_WAIT, read once: whether a present waits for the main thread to set a changed layer
  bool layer_wait_;
  // MULLION_PRESENT_THREAD, read once: whether a present takes its drawable on the presenter's thread
  bool thread_on_;
  double native_contents_scale_ = 1.0;
  uint32_t sample_count_;
  WMTColorSpace colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata hdr_metadata_;
  bool has_hdr_metadata_ = false;
  DXGI_FORMAT source_format_ = DXGI_FORMAT_UNKNOWN;
  uint64_t display_setting_version_ = 0;
  WMTColorSpace display_colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata display_hdr_metadata_;
  WMTEDRValue display_edr_value_{0.0, 1.0};
  MullionPresentation presentation_;
  uint64_t gamma_version_ = 0;
  std::array<float, MULLION_GAMMA_CP_COUNT * 4> gamma_lut_rgba_;
  WMT::Reference<WMT::Texture> gamma_lut_texture_;
  WMT::Reference<WMT::RenderPipelineState> present_blit_;
  WMT::Reference<WMT::RenderPipelineState> present_scale_;
  std::atomic_flag pso_valid = 0;
  HWND window_;
  WMT::Reference<WMT::Texture> window_target_;
  struct WindowFrame {
    // placed here: a 32-bit process reads no memory Metal allocates
    std::unique_ptr<void, void (*)(void *)> pixels{nullptr, wsi::aligned_free};
    WMT::Reference<WMT::Buffer> buffer;
    uint32_t width, height;
    uint64_t number;
  };
  WMT::Reference<WMT::PresentStatistics> statistics_ = WMT::MakePresentStatistics();
  // the frames encoded
  uint64_t frames_ = 0;
  std::deque<WindowFrame> window_frames_;
  std::chrono::steady_clock::time_point window_last_draw_;
  mullion::mutex window_mutex_;

  // MULLION_PRESENT_THREAD: what the presenter and its thread share, made at the first present that defers. The thread
  // keeps it after the presenter is gone (a detached thread, which ends at its next Mailbox::Wait), so that nothing
  // the thread uses, the layer included, goes before it does
  struct Shared {
    // a frame texture, and the minimum duration the frame drawn into it last is to be presented after
    struct Payload {
      WMT::Reference<WMT::Texture> texture;
      double after = 0;
    };
    present_frames::Mailbox<mullion::mutex, mullion::condition_variable, Payload> mailbox;
    // retained: the bridge releases a view's layer on the main thread as the swap chain goes, maybe while the thread
    // waits for a drawable of it
    WMT::Reference<WMT::MetalLayer> layer;
    // the presenter's own, which GetFrameStatistics reads
    WMT::Reference<WMT::PresentStatistics> statistics;
    // the thread's own Metal 3 queue
    WMT::Reference<WMT::CommandQueue> queue;
    // the thread's time in nextDrawable since the last present encoded, for HITCH's drawableMs
    std::atomic<uint64_t> drawable_wait_ns{0};
  };
  static void presentLoop(std::shared_ptr<Shared> shared);
  std::shared_ptr<Shared> shared_;
};
} // namespace mullion