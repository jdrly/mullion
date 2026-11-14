#pragma once

// jdrly/mullion: Metal's lossless compression of the textures the bridge makes (docs/swtor/README.md). Apple Silicon
// compresses render targets and sampled textures by default; allowGPUOptimizedContents NO turns it off on every
// family. The bridge turns it off for a texture its caller asks for so (WMTTextureInfo.uncompressed) and, with
// MULLION_TEX_NO_COMPRESSION=1, for every texture; otherwise it leaves the descriptor as Metal made it, so such a
// texture is made as before. In plain Objective-C over Metal, so that tests/host/texture_compression.c runs it
// natively. Include winemetal.h first, for struct WMTTextureInfo.

#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// MULLION_TEX_NO_COMPRESSION, by the grammar of the switch d3d12's device declares for the session header
// (d3d12_device.cpp): on only for exactly "1", DXMT_TEX_NO_COMPRESSION when it is unset or empty
static inline bool
texture_compression_off_by_switch(void) {
  const char *value = getenv("MULLION_TEX_NO_COMPRESSION");
  if (!value || !*value)
    value = getenv("DXMT_TEX_NO_COMPRESSION");
  return value && !strcmp(value, "1");
}

// `all`: MULLION_TEX_NO_COMPRESSION, as read once by the bridge
static inline void
texture_compression_apply(MTLTextureDescriptor *desc, const struct WMTTextureInfo *info, bool all) {
  if (info->uncompressed || all)
    desc.allowGPUOptimizedContents = NO;
}
