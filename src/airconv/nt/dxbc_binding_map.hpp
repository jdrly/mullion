#pragma once

#include "air_builder.hpp"
#include "llvm/IR/Value.h"
#include <optional>

namespace mullion::dxbc {

struct ConstantBufferDescriptor {
  llvm::Value *Pointer;
  llvm::Value *Metadata; // may be null
  // jdrly/mullion: the SM50_CHECK_KIND a failed check of its bounds counts as, where a flag added the check
  std::optional<uint32_t> Counted = std::nullopt;
};

struct SamplerDescriptor {
  llvm::Value *SamplerHandle;
  llvm::Value *CubeSamplerHandle;
  llvm::Value *Metadata;
  llvm::Value *Border = nullptr; // SM50_SAMPLER_BORDER's address, when the binding can have one
  // a static sampler's, known when the pipeline is made: whether it filters anisotropically, whether its metadata
  // holds a LOD bias, and whether it is a minimum or maximum filter
  std::optional<bool> Anisotropic, Biased, Reduces;
};

struct TextureDescirptor {
  llvm::Value *ResourceHandle;
  llvm::Value *Metadata;
  bool GlobalCoherent;
  llvm::air::Texture::ResourceKind ResourceKind;
  llvm::air::Texture::ResourceKind ResourceKindLogical;
  llvm::air::Texture::MemoryAccess MemoryAccess;
  llvm::air::Texture::SampleType SampleType;
  llvm::Value *Range = nullptr; // where its format range is (SM50_SAMPLER_BORDER), when the binding has one
  bool IsUAV = false;
};

struct BufferDescriptor {
  llvm::Value *Pointer;
  llvm::Value *Metadata;
  uint32_t StructureStride;
  bool GlobalCoherent;
  // whether an access out of its bounds is defined: not through a root descriptor, nor through a descriptor root
  // signature 1.1 says is static ("Out of bounds accesses to any Buffer views ... are invalid and produce undefined
  // results", Root Signature Version 1.1, Static and Volatile Flags)
  bool Bounded = true;
  // jdrly/mullion: where a load whose bounds check fails reads, and drops, instead of the view's start, null for the
  // start: a root view's (SM50_SHADER_FLAG_CHECK_ROOT_RANGES), whose start may have fewer bytes than one load
  llvm::Value *Fallback = nullptr;
  // jdrly/mullion: the SM50_CHECK_KIND a failed check of its bounds counts as, where a flag added the check: a static
  // range's view (SM50_SHADER_FLAG_GUARD_STATIC_RANGES) or a root view's (SM50_SHADER_FLAG_CHECK_ROOT_RANGES)
  std::optional<uint32_t> Counted = std::nullopt;
};

struct CounterDescriptor {
  llvm::Value *Pointer;
};

using RangeId = uint32_t;

class BindingMap {
public:
  virtual ~BindingMap() {};
  virtual llvm::Optional<ConstantBufferDescriptor>
  GetConstantBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<SamplerDescriptor>
  GetSampler(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<TextureDescirptor>
  GetSRVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<TextureDescirptor>
  GetUAVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<BufferDescriptor>
  GetSRVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<BufferDescriptor>
  GetUAVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  virtual llvm::Optional<CounterDescriptor>
  GetUAVCounter(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) = 0;
  // the device's table of lock words for 64-bit atomics (word 0 its index mask), where the binding carries one
  virtual llvm::Value *
  GetAtomicLocks(llvm::air::AIRBuilder &Builder) {
    return nullptr;
  }
  virtual llvm::Value *
  GetSparseBufferMappings(llvm::air::AIRBuilder &Builder) {
    return nullptr;
  }
};

// jdrly/mullion: a check of `Kind` (SM50_CHECK_KIND) that fails where `Within` is false is counted in its word of the
// lock table, once per call of the function it is in: FinishCheckCounts gathers a function's checks into one value,
// which each of its returns adds to the words of the kinds that failed, behind one branch. The access takes no branch,
// and a check that passes costs a select and an or. Nothing where the binding has no lock table, or in a pass whose
// writes are discarded
void CountFailedCheck(BindingMap &Binding, llvm::air::AIRBuilder &AIR, llvm::Value *Within, uint32_t Kind);
// before the optimizer (runOptimizationPasses): without it, at -O0, each check adds to its word where it fails
void FinishCheckCounts(llvm::Module &M);

} // namespace mullion::dxbc