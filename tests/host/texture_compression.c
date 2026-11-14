// contract: the bridge turns Metal's lossless compression off for a texture whose WMTTextureInfo asks for it
// (uncompressed = 1) or for every texture with MULLION_TEX_NO_COMPRESSION=1, and otherwise leaves the descriptor as
// Metal made it, compression allowed (src/winemetal/unix/texture_compression.h). where the machine has a Metal device,
// a 4x MSAA target and a single-sampled one are made each way: the one asked for has to come back without compression.
// without a device only the descriptors are checked, and the test says so
#include <stdio.h>
#include <stdlib.h>
#import <Foundation/Foundation.h>
#define WINEMETAL_API
#include "../../src/winemetal/winemetal.h"
#include "texture_compression.h"

static unsigned failures = 0;

static void
check(bool ok, const char *what) {
  if (!ok)
    failures++, printf("failed: %s\n", what);
}

// the descriptor fill_texture_descriptor gives a render target of `samples` samples, with `info` and `all`
static MTLTextureDescriptor *
descriptor(const struct WMTTextureInfo *info, unsigned samples, bool all) {
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  desc.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
  desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
  desc.width = desc.height = 64;
  desc.sampleCount = samples;
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModePrivate;
  texture_compression_apply(desc, info, all);
  return desc;
}

int
main(void) {
  @autoreleasepool {
    struct WMTTextureInfo asked = {.uncompressed = 1}, left = {0};
    check(sizeof(struct WMTTextureInfo) == 56, "WMTTextureInfo is 56 bytes, as the Windows side has it");

    check([[[MTLTextureDescriptor alloc] init] autorelease].allowGPUOptimizedContents,
          "Metal's own descriptor allows compression");
    for (unsigned samples = 1; samples <= 4; samples *= 4) {
      check([descriptor(&left, samples, false) autorelease].allowGPUOptimizedContents,
            "a texture nobody asks about keeps Metal's default");
      check(![descriptor(&asked, samples, false) autorelease].allowGPUOptimizedContents,
            "uncompressed = 1 turns compression off");
      check(![descriptor(&left, samples, true) autorelease].allowGPUOptimizedContents,
            "MULLION_TEX_NO_COMPRESSION=1 turns it off for every texture");
    }

    // the switch's grammar: exactly 1, MULLION_ before DXMT_
    unsetenv("DXMT_TEX_NO_COMPRESSION");
    setenv("MULLION_TEX_NO_COMPRESSION", "1", 1);
    check(texture_compression_off_by_switch(), "MULLION_TEX_NO_COMPRESSION=1 is on");
    setenv("MULLION_TEX_NO_COMPRESSION", "true", 1);
    check(!texture_compression_off_by_switch(), "MULLION_TEX_NO_COMPRESSION=true is off");
    unsetenv("MULLION_TEX_NO_COMPRESSION");
    check(!texture_compression_off_by_switch(), "unset is off");
    setenv("DXMT_TEX_NO_COMPRESSION", "1", 1);
    check(texture_compression_off_by_switch(), "DXMT_TEX_NO_COMPRESSION=1 is on");
    setenv("MULLION_TEX_NO_COMPRESSION", "0", 1);
    check(!texture_compression_off_by_switch(), "MULLION_ wins over DXMT_");
    unsetenv("MULLION_TEX_NO_COMPRESSION");
    unsetenv("DXMT_TEX_NO_COMPRESSION");

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      printf("no Metal device: the descriptors only\n");
    } else {
      printf("Metal device: %s\n", device.name.UTF8String);
      for (unsigned samples = 1; samples <= 4; samples *= 4) {
        if (![device supportsTextureSampleCount:samples])
          continue;
        id<MTLTexture> kept = [device newTextureWithDescriptor:[descriptor(&left, samples, false) autorelease]];
        id<MTLTexture> plain = [device newTextureWithDescriptor:[descriptor(&asked, samples, false) autorelease]];
        check(kept && plain, "Metal makes both textures");
        if (kept && plain) {
          printf("%u samples: allowGPUOptimizedContents %d left, %d asked\n", samples, kept.allowGPUOptimizedContents,
                 plain.allowGPUOptimizedContents);
          // whether a device compresses at all is its own: a virtual one may not, so the texture left alone is
          // printed, not checked
          check(!plain.allowGPUOptimizedContents, "the texture asked for is made without compression");
        }
        [kept release];
        [plain release];
      }
      [device release];
    }
  }
  if (failures)
    printf("%u failed\n", failures);
  return failures ? 1 : 0;
}
