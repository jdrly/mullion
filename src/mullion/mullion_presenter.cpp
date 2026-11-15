
#include <algorithm>
#include "Metal.hpp"
#include "mullion_format.hpp"
#include "mullion_presenter.hpp"
#include "mullion_diag.hpp"
#include "util_likely.hpp"
#include "util_math.hpp"
#include "util_env.hpp"
#include "util_switch.hpp"


namespace mullion {

// 1: a present that changes its layer waits for the main thread to set it, as upstream does. 0: it goes on, and the
// main thread sets it later, in the order asked (src/winemetal/unix/layer.c)
static sw::Switch<bool> kLayerWait{"MULLION_LAYER_WAIT", true, sw::notZero};
// 1: a Direct3D 12 present takes its drawable on the presenter's own thread, after the frame was drawn into a texture
// of the presenter's on the queue's command buffer, so a nextDrawable that blocks (about 1.2 s at a switch away from
// the window's Space) holds up no command buffer and no fence. 0: on the queue's commit thread, as before
static sw::Switch<bool> kPresentThread{"MULLION_PRESENT_THREAD", true, sw::notZero};

Presenter::Presenter(WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count, HWND window) :
    device_(device),
    layer_(layer),
    lib_(lib),
    layer_wait_(kLayerWait()),
    thread_on_(kPresentThread()),
    sample_count_(sample_count),
    window_(window) {
  // MULLION_LAYER_WAIT=0 is new behaviour: the log says so, once per process
  static std::atomic_bool said;
  if (!layer_wait_ && !said.exchange(true))
    Logger::info("Mullion: layer wait=0");
  // MULLION_PRESENT_THREAD=0 brings back the stall at a Space switch: the log says so, once per process
  static std::atomic_bool said_thread;
  if (!thread_on_ && !said_thread.exchange(true))
    Logger::info("Mullion: present thread=0");
  layer_.getProps(layer_props_);
  native_contents_scale_ = layer_props_.contents_scale;
  layer_props_.device = device;
  layer_props_.opaque = true;
  layer_props_.display_sync_enabled = false;
  layer_props_.framebuffer_only = false; // how strangely setting it true results in worse performance
  layer_props_.contents_scale = layer_props_.contents_scale * scale_factor;
}

Presenter::~Presenter() {
  if (!thread_.joinable())
    return;
  {
    std::lock_guard lock(present_mutex_);
    stopping_ = true;
  }
  present_changed_.notify_all();
  thread_.join();
}

bool
Presenter::upscaleOutputSize(uint32_t input_width, uint32_t input_height, uint32_t &width, uint32_t &height) {
  WMTLayerProps props{};
  if (!layer_)
    return false;
  layer_.getProps(props);
  double scale = native_contents_scale_ > 0 ? native_contents_scale_ : 1.0;
  uint32_t native_width = uint32_t(props.bounds_width * scale + 0.5);
  uint32_t native_height = uint32_t(props.bounds_height * scale + 0.5);
  if (native_width < input_width || native_height < input_height)
    return false;
  width = native_width;
  height = native_height;
  return true;
}

bool
Presenter::changeLayerProperties(
    DXGI_FORMAT format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
) {
  bool should_invalidated = colorspace_ != colorspace;
  should_invalidated |= source_format_ != format;
  source_format_ = format;
  should_invalidated |= sample_count_ != sample_count;
  sample_count_ = sample_count;
  // a 32-bit GDI bitmap is BGRA
  layer_props_.pixel_format = layer_ ? Forget_sRGB(ConvertSwapChainFormat(format)) : WMTPixelFormatBGRA8Unorm;
  layer_props_.drawable_height = height;
  layer_props_.drawable_width = width;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear(); // defer changes
  return should_invalidated;
}

bool
Presenter::changeLayerColorSpace(WMTColorSpace colorspace) {
  bool should_invalidated = colorspace_ != colorspace;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear();
  return should_invalidated;
}

void
Presenter::changeHDRMetadata(const WMTHDRMetadata *metadata) {
  if (metadata) {
    has_hdr_metadata_ = true;
    hdr_metadata_ = *metadata;
    pso_valid.clear();
  } else {
    if (has_hdr_metadata_) {
      has_hdr_metadata_ = false;
      pso_valid.clear();
    }
  }
}

void
Presenter::changeGammaRamp(const MullionGammaRamp *gamma_ramp) {
  if (!gamma_ramp && gamma_version_ != 0) {
    gamma_version_ = 0;
    pso_valid.clear();
    return;
  }
  if (gamma_ramp && gamma_version_ != gamma_ramp->version) {
    gamma_version_ = gamma_ramp->version;
    for (uint32_t i = 0; i < MULLION_GAMMA_CP_COUNT; i++) {
      gamma_lut_rgba_[i * 4 + 0] = std::clamp(gamma_ramp->red[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 1] = std::clamp(gamma_ramp->green[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 2] = std::clamp(gamma_ramp->blue[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 3] = 1.0f;
    }
    WMTTextureInfo texture_info{};
    texture_info.type = WMTTextureType2D;
    texture_info.pixel_format = WMTPixelFormatRGBA32Float;
    texture_info.usage = WMTTextureUsageShaderRead;
    texture_info.options = WMTResourceStorageModeShared;
    texture_info.width = MULLION_GAMMA_CP_COUNT;
    texture_info.height = 1;
    texture_info.depth = 1;
    texture_info.mipmap_level_count = 1;
    texture_info.sample_count = 1;
    texture_info.array_length = 1;
    gamma_lut_texture_ = device_.newTexture(texture_info);
    gamma_lut_texture_.replaceRegion(
          {0, 0, 0}, {MULLION_GAMMA_CP_COUNT, 1, 1}, 0, 0,
          gamma_lut_rgba_.data(),
          MULLION_GAMMA_CP_COUNT * sizeof(float) * 4,
          0);
    pso_valid.clear();
  }
}

Presenter::PresentState
Presenter::synchronizeLayerProperties() {
  uint64_t display_setting_version = 0;

  WMTQueryDisplaySettingForLayer(
      layer_.handle, &display_setting_version, &display_colorspace_, &display_hdr_metadata_, &display_edr_value_
  );

  if (display_setting_version != display_setting_version_) {
    display_setting_version_ = display_setting_version;
    pso_valid.clear();
  }

  auto final_colorspace = display_setting_version_ > 0 ? display_colorspace_ : colorspace_;
  auto is_hdr = WMT_COLORSPACE_IS_HDR(final_colorspace);
  auto hdr_metadata = display_setting_version_ > 0 ? &display_hdr_metadata_
                      : has_hdr_metadata_          ? &hdr_metadata_
                                                   : nullptr;
  if (unlikely(!pso_valid.test_and_set()))
    buildRenderPipelineState(
        final_colorspace == WMTColorSpaceHDR_PQ, is_hdr && hdr_metadata != nullptr, sample_count_ > 1,
        gamma_version_ != 0
    );

  MullionPresentMetadata metadata;

  metadata.presentation = presentation_;
  metadata.edr_scale = 1.0;
  metadata.max_content_luminance = 10000;
  metadata.max_display_luminance = display_edr_value_.maximum_potential_edr_color_component_value * 100;

  if (is_hdr) {
    metadata.edr_scale = display_edr_value_.maximum_edr_color_component_value /
                display_edr_value_.maximum_potential_edr_color_component_value;

    if (hdr_metadata) {
      metadata.max_content_luminance = std::max<uint32_t>(
          {(uint32_t)metadata.max_display_luminance, hdr_metadata->max_content_light_level,
           hdr_metadata->max_mastering_luminance, hdr_metadata->max_frame_average_light_level}
      );
    }
  }

  if (final_colorspace == WMTColorSpaceHDR_scRGB)
    metadata.edr_scale *= 0.8;

  return {metadata, layer_props_, final_colorspace, gamma_lut_texture_, present_blit_, present_scale_};
}

template <typename CommandBuffer, typename Encoder>
WMT::MetalDrawable
Presenter::encodeCommands(
    CommandBuffer cmdbuf, WMT::Texture backbuffer, const PresentState &state,
    std::type_identity_t<std::function<void(Encoder)>> &&wait_fences,
    std::type_identity_t<std::function<void(Encoder)>> &&update_fences, uint64_t seq_id, unsigned *encoder_count,
    uint64_t *drawable_ns, Deferred *deferred
) {
  const auto &metadata = state.metadata;
  double width = state.layer_props.drawable_width;
  double height = state.layer_props.drawable_height;
  layer_.setProps(state.layer_props, layer_wait_);
  layer_.setColorSpace(state.colorspace, layer_wait_);

  if (drawable_ns)
    *drawable_ns = 0;
  // MULLION_PRESENT_THREAD: no drawable here; the presenter's thread takes it (presentLoop)
  const bool threaded = deferred && threaded();
  WMT::MetalDrawable drawable{};
  if (threaded) {
    if (drawable_ns)
      *drawable_ns = drawable_wait_ns_.exchange(0, std::memory_order_relaxed);
  } else if (layer_)
    drawable = layer_.nextDrawable(drawable_ns);
  auto number = ++frames_;
  if (drawable)
    drawable.countPresent(statistics_, number);
  if (threaded) {
    std::lock_guard lock(present_mutex_);
    bool grew;
    unsigned index = book_.Draw(number, grew);
    if (grew) {
      frame_textures_.emplace_back();
      shown_.emplace_back();
    }
    auto &texture = frame_textures_[index];
    // a texture of another size or format is made again; a command buffer still drawing into the one before keeps it
    // (Deferred::texture)
    if (!texture || texture.width() != width || texture.height() != height ||
        texture.pixelFormat() != state.layer_props.pixel_format) {
      WMTTextureInfo texture_info{};
      texture_info.type = WMTTextureType2D;
      texture_info.pixel_format = state.layer_props.pixel_format;
      texture_info.usage = WMTTextureUsageRenderTarget;
      texture_info.options = WMTResourceStorageModePrivate;
      texture_info.width = width;
      texture_info.height = height;
      texture_info.depth = 1;
      texture_info.mipmap_level_count = 1;
      texture_info.sample_count = 1;
      texture_info.array_length = 1;
      texture = device_.newTexture(texture_info);
    }
    *deferred = {index, number, texture};
    if (!thread_.joinable()) {
      present_queue_ = device_.newCommandQueue(4);
      thread_ = mullion::thread([this] { presentLoop(); });
    }
  } else if (!drawable && (!window_target_ || window_target_.width() != width || window_target_.height() != height)) {
    WMTTextureInfo texture_info{};
    texture_info.type = WMTTextureType2D;
    texture_info.pixel_format = state.layer_props.pixel_format;
    texture_info.usage = WMTTextureUsageRenderTarget;
    texture_info.options = WMTResourceStorageModePrivate;
    texture_info.width = width;
    texture_info.height = height;
    texture_info.depth = 1;
    texture_info.mipmap_level_count = 1;
    texture_info.sample_count = 1;
    texture_info.array_length = 1;
    window_target_ = device_.newTexture(texture_info);
  }
  WMT::Texture target = drawable ? drawable.texture() : threaded ? deferred->texture : window_target_;
  // a size the main thread hasn't set yet isn't the drawable's: draw to the size it has
  if (!layer_wait_ && drawable) {
    width = target.width();
    height = target.height();
  }

  WMTRenderPassInfo info;
  WMT::InitializeRenderPassInfo(info);
  info.colors[0].load_action = WMTLoadActionDontCare;
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].texture = target;
  auto encoder = cmdbuf.renderCommandEncoder(info);
  if (encoder_count) {
    auto index = (*encoder_count)++;
    if (diag::errors || diag::trace_encoders) {
      auto label = str::format(
          "present ", seq_id, ".", index, ": ", target.width(), "x", target.height(),
          ", 1 draws (0 geometry, 0 tessellation)"
      );
      if (diag::errors)
        encoder.setLabel(WMT::String::string(label.c_str(), WMTUTF8StringEncoding));
      if (diag::trace_encoders)
        Logger::info("encoder " + label);
    }
  }
  wait_fences(encoder);
  encoder.setFragmentTexture(backbuffer, 0);
  encoder.setFragmentTexture(state.gamma_lut, 1);

  encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
  // a back buffer as large as the target is read texel for texel, when all of it is presented as it is
  if (backbuffer.width() == (uint64_t)width && backbuffer.height() == (uint64_t)height && metadata.presentation.plain()) {
    encoder.setRenderPipelineState(state.blit);
  } else {
    encoder.setRenderPipelineState(state.scale);
  }
  encoder.setViewport({0, 0, width, height, 0, 1});
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  update_fences(encoder);
  encoder.endEncoding();

  // a layer whose drawable timed out drops this frame; only a window without a layer takes frames through GDI
  if (!layer_) {
    WindowFrame frame{.width = (uint32_t)width, .height = (uint32_t)height, .number = number};
    WMTBufferInfo buffer_info{
        .length = align(frame.width * frame.height * 4ull, MULLION_PAGE_SIZE), .options = WMTResourceStorageModeShared
    };
    frame.pixels.reset(wsi::aligned_malloc(buffer_info.length, MULLION_PAGE_SIZE));
    buffer_info.memory.set(frame.pixels.get());
    frame.buffer = device_.newBuffer(buffer_info);
    wmtcmd_blit_copy_from_texture_to_buffer copy{.type = WMTBlitCommandCopyFromTextureToBuffer};
    copy.next.set(nullptr);
    copy.src = target.handle;
    copy.size = {frame.width, frame.height, 1};
    copy.dst = frame.buffer.handle;
    copy.bytes_per_row = frame.width * 4;
    copy.bytes_per_image = frame.width * frame.height * 4;
    auto blit = cmdbuf.blitCommandEncoder();
    if (encoder_count)
      (*encoder_count)++;
    blit.encodeCommands((const wmtcmd_blit_nop *)&copy);
    blit.endEncoding();
    std::lock_guard lock(window_mutex_);
    window_frames_.push_back(std::move(frame));
  }
  return drawable;
}

template WMT::MetalDrawable Presenter::encodeCommands(
    WMT::CommandBuffer, WMT::Texture, const PresentState &, std::function<void(WMT::RenderCommandEncoder)> &&,
    std::function<void(WMT::RenderCommandEncoder)> &&, uint64_t, unsigned *, uint64_t *, Deferred *
);
template WMT::MetalDrawable Presenter::encodeCommands(
    WMT::Metal4CommandBuffer, WMT::Texture, const PresentState &,
    std::function<void(WMT::Metal4RenderCommandEncoder)> &&, std::function<void(WMT::Metal4RenderCommandEncoder)> &&,
    uint64_t, unsigned *, uint64_t *, Deferred *
);

void
Presenter::show(const Deferred &deferred, double after) {
  std::lock_guard lock(present_mutex_);
  if (!book_.Done(deferred.index, deferred.frame))
    return;
  shown_[deferred.index] = {after};
  present_changed_.notify_all();
}

void
Presenter::presentLoop() {
  env::setThreadName("mullion-present");
  // the frames dropped since the last line, and the longest wait for a drawable among them
  uint64_t dropped = 0, longest_ns = 0;
  auto said = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  for (;;) {
    unsigned index;
    uint64_t number;
    double after;
    WMT::Reference<WMT::Texture> texture;
    {
      std::unique_lock lock(present_mutex_);
      present_changed_.wait(lock, [&] { return stopping_ || book_.HasReady(); });
      if (stopping_)
        return;
      auto taken = book_.Take();
      index = taken->first;
      number = taken->second;
      after = shown_[index].after;
      texture = frame_textures_[index];
    }
    auto pool = WMT::MakeAutoreleasePool();
    uint64_t waited = 0;
    // the call that blocks at a Space switch: here it holds up only this thread
    auto drawable = layer_.nextDrawable(&waited);
    drawable_wait_ns_.fetch_add(waited, std::memory_order_relaxed);
    longest_ns = std::max(longest_ns, waited);
    bool shown = false;
    if (drawable) {
      auto target = drawable.texture();
      // a layer the main thread resized or reformatted since the frame was drawn: the part both have, or nothing for
      // another format, which a copy can't take
      if (target.pixelFormat() == texture.pixelFormat()) {
        drawable.countPresent(statistics_, number);
        auto cmdbuf = present_queue_.commandBuffer();
        wmtcmd_blit_copy_from_texture_to_texture copy{.type = WMTBlitCommandCopyFromTextureToTexture};
        copy.next.set(nullptr);
        copy.src = texture.handle;
        copy.src_size = {uint32_t(std::min(texture.width(), target.width())),
                         uint32_t(std::min(texture.height(), target.height())), 1};
        copy.dst = target.handle;
        auto blit = cmdbuf.blitCommandEncoder();
        blit.encodeCommands((const wmtcmd_blit_nop *)&copy);
        blit.endEncoding();
        if (after > 0)
          cmdbuf.presentDrawableAfterMinimumDuration(drawable, after);
        else
          cmdbuf.presentDrawable(drawable);
        cmdbuf.commit();
        // the texture is free once the copy has read it
        cmdbuf.waitUntilCompleted();
        shown = true;
      }
    }
    std::lock_guard lock(present_mutex_);
    book_.Release(index, shown);
    dropped += book_.TakeDropped();
    // the frames lost while this thread waited for a drawable or fell behind: at most a line a second
    auto now = std::chrono::steady_clock::now();
    if (dropped && now - said >= std::chrono::seconds(1)) {
      SYSTEMTIME local;
      GetLocalTime(&local);
      char at[16];
      std::snprintf(at, sizeof(at), "%02u:%02u:%02u.%03u", local.wHour, local.wMinute, local.wSecond,
                    local.wMilliseconds);
      Logger::info(str::format("Mullion: present dropped=", dropped, " drawableMs=", longest_ns / 1000000, " at=", at));
      dropped = longest_ns = 0;
      said = now;
    } else if (!dropped)
      longest_ns = 0;
  }
}

void
Presenter::drawToWindow(double after) {
  WindowFrame frame;
  {
    std::lock_guard lock(window_mutex_);
    if (window_frames_.empty())
      return;
    frame = std::move(window_frames_.front());
    window_frames_.pop_front();
  }
  std::this_thread::sleep_until(window_last_draw_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                        std::chrono::duration<double>(after)
                                                    ));
  // a top-down bitmap: negative height
  BITMAPINFO bitmap{{sizeof(BITMAPINFOHEADER), (LONG)frame.width, -(LONG)frame.height, 1, 32, BI_RGB}};
  // the window may be gone, and its own DC may be in use on its thread (CS_OWNDC): a cache DC, or no frame
  RECT client;
  HDC dc;
  if (!GetClientRect(window_, &client) || !(dc = GetDCEx(window_, nullptr, DCX_CACHE)))
    return;
  SetStretchBltMode(dc, COLORONCOLOR);
  StretchDIBits(
      dc, 0, 0, client.right, client.bottom, 0, 0, frame.width, frame.height, frame.pixels.get(), &bitmap, DIB_RGB_COLORS,
      SRCCOPY
  );
  ReleaseDC(window_, dc);
  window_last_draw_ = std::chrono::steady_clock::now();
  WMT::MetalDrawable{}.countPresent(statistics_, frame.number);
}

HRESULT
Presenter::frameStatistics(DXGI_FRAME_STATISTICS *stats, double refresh_rate) {
  LARGE_INTEGER before, after, frequency;
  QueryPerformanceCounter(&before);
  auto shown = statistics_.get();
  QueryPerformanceCounter(&after);
  QueryPerformanceFrequency(&frequency);
  if (!shown.shown)
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
  // the host's clock was read between the two QPC reads
  auto qpc = (before.QuadPart + after.QuadPart) / 2 - LONGLONG((shown.now - shown.shown_time) * frequency.QuadPart);
  // the scheduler's last sample is the refresh the frame was shown at
  UINT refresh = UINT(std::llround((shown.shown_time - shown.since) * refresh_rate));
  *stats = {UINT(shown.shown), refresh, refresh, {.QuadPart = qpc}, {}};
  return S_OK;
}

void
Presenter::buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable) {
  auto pool = WMT::MakeAutoreleasePool();

  auto library = lib_.getLibrary();

  uint32_t true_data = true, false_data = false;
  WMTFunctionConstant constants[] = {
      {{&true_data}, WMTDataTypeBool, kPresentFCIndex_BackbufferSizeMatched},
      {{is_pq ? &true_data : &false_data}, WMTDataTypeBool, kPresentFCIndex_HDRPQ},
      {{with_hdr_metadata ? &true_data : &false_data}, WMTDataTypeBool, kPresentFCIndex_WithHDRMetadata},
      // linear values the target takes as sRGB bytes: an sRGB back buffer's, or a linear color space's in a GDI
      // bitmap, where an sRGB target's own encoding misses D3D11.3 3.2.3.8 by up to 0.8 of a code
      {{Is_sRGBVariant(ConvertSwapChainFormat(source_format_)) || (!layer_ && colorspace_ & WMTColorSpaceSRGBLinear)
            ? &true_data
            : &false_data},
       WMTDataTypeBool, kPresentFCIndex_BackbufferIsSRGB},
      {{is_ms ? &true_data : &false_data}, WMTDataTypeBool, kPresentFCIndex_BackbufferIsMS},
      {{gamma_enable ? &true_data : &false_data}, WMTDataTypeBool, kPresentFCIndex_GammaEnabled},
      {{source_format_ == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM ? &true_data : &false_data}, WMTDataTypeBool,
       kPresentFCIndex_BackbufferIsXRBias},
  };

  WMT::Reference<WMT::Error> error;
  auto vs_present_quad = library.newFunction("vs_present_quad");
  auto fs_present_quad = library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);

  constants[0].data.set(&false_data);
  auto fs_present_quad_scaled =
      library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);
  {
    WMTRenderPipelineInfo present_pipeline;
    WMT::InitializeRenderPipelineInfo(present_pipeline);
    present_pipeline.colors[0].pixel_format = layer_props_.pixel_format;
    present_pipeline.vertex_function = vs_present_quad;
    present_pipeline.fragment_function = fs_present_quad;
    present_blit_ = device_.newRenderPipelineState(present_pipeline, error);
    present_pipeline.fragment_function = fs_present_quad_scaled;
    present_scale_ = device_.newRenderPipelineState(present_pipeline, error);
  }
}

} // namespace mullion