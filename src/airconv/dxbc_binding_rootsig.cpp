/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "air_signature.hpp"
#include "dxbc_converter.hpp"
#include "dxbc_root_signature.hpp"
#include "shader_common.hpp"
#include "../mullion/mullion_command_constants.hpp"
#include "llvm/IR/DerivedTypes.h"
#include <algorithm>
#include <set>
#include <cmath>
#include <cassert>
#include <cstdlib>
#include <cstring>

namespace mullion::dxbc {

using namespace llvm::air;

// jdrly/mullion: `mullion.count_checks(locks, missed)`: for each kind whose bit `missed` has (SM50_CHECK_KIND), 1 added
// to the kind's word of the lock table, after the mask (word 0), the locks and the flag of an atomic that gave up.
// Nothing when `missed` is 0. Inlined wherever it is called
static llvm::Function *
CountChecks(llvm::air::AIRBuilder &AIR, llvm::Type *Locks) {
  auto &B = AIR.builder;
  auto &M = *B.GetInsertBlock()->getModule();
  if (auto Fn = M.getFunction("mullion.count_checks"))
    return Fn;
  auto Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {Locks, B.getInt32Ty()}, false), llvm::GlobalValue::InternalLinkage,
      "mullion.count_checks", M
  );
  Fn->addFnAttr(llvm::Attribute::AlwaysInline);
  auto Table = Fn->getArg(0), Missed = Fn->getArg(1);
  auto Block = [&](const char *Name) { return llvm::BasicBlock::Create(M.getContext(), Name, Fn); };
  llvm::IRBuilderBase::InsertPointGuard Guard(B);
  B.SetInsertPoint(Block("entry"));
  auto Done = Block("done"), Count = Block("count");
  B.CreateCondBr(B.CreateICmpEQ(Missed, B.getInt32(0)), Done, Count);
  B.SetInsertPoint(Count);
  auto Mask = B.CreateLoad(B.getInt32Ty(), Table);
  for (uint32_t Kind = 0; Kind < SM50_CHECK_KINDS; Kind++) {
    auto Add = Block("count_kind"), Next = Block("next_kind");
    B.CreateCondBr(B.CreateICmpNE(B.CreateAnd(Missed, B.getInt32(1u << Kind)), B.getInt32(0)), Add, Next);
    B.SetInsertPoint(Add);
    auto Word = B.CreateGEP(B.getInt32Ty(), Table, {B.CreateAdd(Mask, B.getInt32(3 + Kind))});
    // the word is the device's own, which nothing the shader reads holds: the optimizer may keep values it loaded
    // before, which an atomic that may write any memory would make it load again
    llvm::cast<llvm::CallInst>(AIR.CreateAtomicRMW(llvm::AtomicRMWInst::Add, Word, B.getInt32(1)))
        ->addFnAttr(llvm::Attribute::InaccessibleMemOnly);
    B.CreateBr(Next);
    B.SetInsertPoint(Next);
  }
  B.CreateBr(Done);
  B.SetInsertPoint(Done);
  B.CreateRetVoid();
  return Fn;
}

void
CountFailedCheck(BindingMap &Binding, llvm::air::AIRBuilder &AIR, llvm::Value *Within, uint32_t Kind) {
  // MULLION_AIRCONV_SKIP=check-counts leaves the counting out, to measure what it costs (it is in the shader cache's key)
  static const bool skip = [] {
    auto names = getenv("MULLION_AIRCONV_SKIP") ?: getenv("DXMT_AIRCONV_SKIP");
    return names && strstr(names, "check-counts");
  }();
  // a geometry shader's counting pass for stream output runs every access again in the pass that writes: it counts there
  if (skip || AIR.DiscardWrites)
    return;
  auto Locks = Binding.GetAtomicLocks(AIR);
  if (!Locks)
    return;
  auto &B = AIR.builder;
  B.CreateCall(
      CountChecks(AIR, Locks->getType()), {Locks, B.CreateSelect(Within, B.getInt32(0), B.getInt32(1u << Kind))}
  );
}

void
FinishCheckCounts(llvm::Module &M) {
  auto Count = M.getFunction("mullion.count_checks");
  if (!Count)
    return;
  std::map<llvm::Function *, std::vector<llvm::CallInst *>> Sites;
  for (auto User : Count->users())
    if (auto Call = llvm::dyn_cast<llvm::CallInst>(User); Call && Call->getCalledFunction() == Count)
      Sites[Call->getFunction()].push_back(Call);
  auto TyLocks = Count->getFunctionType()->getParamType(0);
  for (auto &[F, Calls] : Sites) {
    llvm::IRBuilder<> B(&*F->getEntryBlock().getFirstInsertionPt());
    auto Locks = B.CreateAlloca(TyLocks), Missed = B.CreateAlloca(B.getInt32Ty());
    B.CreateStore(llvm::Constant::getNullValue(TyLocks), Locks);
    B.CreateStore(B.getInt32(0), Missed);
    for (auto Call : Calls) {
      B.SetInsertPoint(Call);
      B.CreateStore(Call->getArgOperand(0), Locks);
      B.CreateStore(B.CreateOr(B.CreateLoad(B.getInt32Ty(), Missed), Call->getArgOperand(1)), Missed);
      Call->eraseFromParent();
    }
    for (auto &Block : *F)
      if (auto Ret = llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator())) {
        B.SetInsertPoint(Ret);
        B.CreateCall(Count, {B.CreateLoad(TyLocks, Locks), B.CreateLoad(B.getInt32Ty(), Missed)});
      }
  }
}

class RootSignatureBindingMap : public BindingMap {
public:
  // the root arguments and the static samplers are arguments of the function, or what a ray tracing shader took
  // from its context (bind_rootsig_arguments)
  llvm::Value *
  Table(llvm::air::AIRBuilder &AIR, uint32_t TableIndex) {
    if (TableIndex == RootSignatureArgumentIndex && RootArguments)
      return RootArguments;
    if (TableIndex == StaticSamplerArgumentIndex && StaticSamplers)
      return StaticSamplers;
    return AIR.builder.GetInsertBlock()->getParent()->getArg(TableIndex);
  }

  llvm::Value *
  GetArgument(llvm::air::AIRBuilder &AIR, uint32_t TableIndex, uint32_t Index) {
    auto &B = AIR.builder;
    auto Arg = Table(AIR, TableIndex);
    auto TyArg = llvm::cast<llvm::PointerType>(Arg->getType())->getNonOpaquePointerElementType();
    auto TyStruct = llvm::cast<llvm::StructType>(TyArg);
    return B.CreateLoad(TyStruct->getElementType(Index), B.CreateStructGEP(TyStruct, Arg, Index));
  };

  // jdrly/mullion: a table's or directly indexed heap's descriptor count in the root arguments
  // (SM50_SHADER_FLAG_CHECK_DESCRIPTOR_INDICES), by the argument the table or heap is; none, no clamp, as upstream
  std::map<uint32_t, uint32_t> DescriptorCounts;

  // the count the descriptors of argument `ArgIndex` are clamped below, or null
  llvm::Value *
  DescriptorCount(llvm::air::AIRBuilder &Builder, uint32_t ArgIndex) {
    auto Count = DescriptorCounts.find(ArgIndex);
    if (Count == DescriptorCounts.end())
      return nullptr;
    return Builder.builder.CreateTrunc(GetArgument(Builder, RootSignatureArgumentIndex, Count->second), Builder.getIntTy());
  }

  // a descriptor index below `Count`: one past it reads the heap's last descriptor, not memory past the heap
  llvm::Value *
  Clamped(llvm::air::AIRBuilder &AIR, llvm::Value *Index, llvm::Value *Count) {
    if (!Count)
      return Index;
    auto &B = AIR.builder;
    auto Within = B.CreateICmpULT(Index, Count);
    CountFailedCheck(*this, AIR, Within, SM50_CHECK_DESCRIPTOR_INDEX);
    return B.CreateSelect(Within, Index, B.CreateSub(Count, AIR.getInt(1)));
  }

  // a constant, structured or raw buffer's pointer and metadata (its byte length). `typed`, for a structured or raw
  // SRV or UAV: the descriptor may be a typed view's, which games give to such slots (Hogwarts Legacy reads tile
  // lists written as RWBuffer<uint> through a typed R32_UINT view as StructuredBuffer<uint>), and drivers read it as
  // the memory it views: one is told by the upper half of its metadata, its element count where a byte length leaves
  // 0, and read from the first byte it keeps in its last qword, 4 bytes an element
  std::pair<llvm::Value *, llvm::Value *>
  GetBufferDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, llvm::Type *HandleType, RangeId RangeId,
      uint32_t DescriptorOffset, bool typed = false, llvm::Value *Count = nullptr
  ) {
    auto TyBufferDescriptor = llvm::StructType::get(
        AIR.getContext(), {HandleType, llvm::Type::getInt64Ty(AIR.getContext()),
                           llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto Pointer = B.CreatePointerCast(
        IntPtr, TyBufferDescriptor->getPointerTo(IntPtr->getType()->getPointerAddressSpace())
    );
    auto IdxDescriptor =
        Clamped(AIR, B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset)), Count);
    auto Handle =
        B.CreateLoad(HandleType, B.CreateGEP(TyBufferDescriptor, Pointer, {IdxDescriptor, AIR.getInt(0) /* pointer */}));
    auto Metadata = B.CreateLoad(
        llvm::Type::getInt64Ty(AIR.getContext()),
        B.CreateGEP(TyBufferDescriptor, Pointer, {IdxDescriptor, AIR.getInt(1) /* metadata */})
    );
    if (!typed)
      return {Handle, Metadata};
    auto Elements = B.CreateLShr(Metadata, 32);
    auto Typed = B.CreateICmpNE(Elements, B.getInt64(0));
    auto First = B.CreateLoad(
        llvm::Type::getInt64Ty(AIR.getContext()),
        B.CreateGEP(TyBufferDescriptor, Pointer, {IdxDescriptor, AIR.getInt(3) /* a typed view's first byte */})
    );
    return {
        B.CreateSelect(Typed, B.CreateIntToPtr(First, HandleType), Handle),
        B.CreateSelect(Typed, B.CreateMul(Elements, B.getInt64(4)), Metadata)
    };
  }

  llvm::Value *
  GetUAVCounterDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, RangeId RangeId, uint32_t DescriptorOffset,
      llvm::Value *Count = nullptr
  ) {
    auto TyCounter = AIR.getIntTy()->getPointerTo(1);
    auto TyBufferDescriptor = llvm::StructType::get(
        AIR.getContext(), {llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext()),
                           TyCounter, llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto Pointer = B.CreatePointerCast(
        IntPtr, TyBufferDescriptor->getPointerTo(IntPtr->getType()->getPointerAddressSpace())
    );
    auto IdxDescriptor =
        Clamped(AIR, B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset)), Count);
    return B.CreateLoad(
        TyCounter, B.CreateGEP(TyBufferDescriptor, Pointer, {IdxDescriptor, AIR.getInt(2) /* metadata */})
    );
  }

  llvm::Value *
  GetRootConstantPointer(llvm::air::AIRBuilder &AIR, uint32_t TableIndex, uint32_t Index) {
    auto &B = AIR.builder;
    auto Arg = Table(AIR, TableIndex);
    auto TyArg = llvm::cast<llvm::PointerType>(Arg->getType())->getNonOpaquePointerElementType();
    auto TyStruct = llvm::cast<llvm::StructType>(TyArg);
    return B.CreatePointerCast(
        B.CreateStructGEP(TyStruct, Arg, Index), AIR.getIntTy(4)->getPointerTo(Arg->getType()->getPointerAddressSpace())
    );
  };

  virtual llvm::Optional<ConstantBufferDescriptor>
  GetConstantBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = ConstantBuffers.find(Range);
    if (Iter == ConstantBuffers.end())
      return {};
    auto &CBuf = Iter->second.first;
    auto DescriptorOffset = Iter->second.second;
    if (DescriptorOffset == ~0u - 1) {
      auto Pointer = GetRootConstantPointer(Builder, RootSignatureArgumentIndex, CBuf.arg_index);
      return ConstantBufferDescriptor{Pointer, nullptr};
    }
    if (DescriptorOffset == ~0u) {
      auto Pointer = GetArgument(Builder, RootSignatureArgumentIndex, CBuf.arg_index);
      auto Length = RootViewLength(Builder, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, Range);
      return ConstantBufferDescriptor{
          Pointer, Length, Length ? std::optional<uint32_t>(SM50_CHECK_ROOT_VIEW) : std::nullopt
      };
    }
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, CBuf.arg_index);
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy(4)->getPointerTo(2), CBuf.range.lower_bound, DescriptorOffset,
        false, DescriptorCount(Builder, CBuf.arg_index)
    );
    return ConstantBufferDescriptor{
        Pointer, Unbounded.count({D3D12_DESCRIPTOR_RANGE_TYPE_CBV, Range}) ? nullptr : Metadata,
        GuardCounted(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, Range)
    };
  }

  SamplerDescriptor
  GetSamplerDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, RangeId RangeId, uint32_t DescriptorOffset,
      llvm::Value *Count = nullptr
  ) {
    auto TySampleHandler = AIR.getSamplerHandleType();
    auto TyInt64 = llvm::Type::getInt64Ty(AIR.getContext());
    auto TySamplerDescriptor =
        llvm::StructType::get(AIR.getContext(), {TySampleHandler, TySampleHandler, TyInt64, TyInt64});
    auto &B = AIR.builder;
    auto IdxDescriptor =
        Clamped(AIR, B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset)), Count);
    auto Field = [&](llvm::Type *Ty, unsigned Field) {
      return B.CreateLoad(
          Ty, B.CreateGEP(
                  TySamplerDescriptor, B.CreatePointerCast(IntPtr, TySamplerDescriptor->getPointerTo(2)),
                  {IdxDescriptor, AIR.getInt(Field)}
              )
      );
    };
    // sampler, cube_sampler, metadata, border
    return {Field(TySampleHandler, 0), Field(TySampleHandler, 1), Field(TyInt64, 2), Field(TyInt64, 3)};
  }

  virtual llvm::Optional<SamplerDescriptor>
  GetSampler(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    auto Iter = Samplers.find(Range);
    if (Iter == Samplers.end()) {
      return {};
    }
    auto &Sampler = Iter->second.first;
    auto DescriptorOffset = Iter->second.second;
    if (DescriptorOffset == ~0u) {
      if (~StaticSamplerArgumentIndex == 0)
        return {};
      auto Static = GetSamplerDescriptor(
          Builder, Table(Builder, StaticSamplerArgumentIndex), Index,
          Sampler.range.lower_bound, Sampler.arg_index
      );
      // a static sampler's border color is one of Metal's own (D3D12_STATIC_BORDER_COLOR)
      Static.Border = nullptr;
      if (auto Known = llvm::dyn_cast<llvm::ConstantInt>(Index)) {
        auto At = Known->getZExtValue() - Sampler.range.lower_bound + Sampler.arg_index;
        if (At < StaticSamplerStates.size())
          std::tie(Static.Anisotropic, Static.Biased, Static.Reduces) = StaticSamplerStates[At];
      }
      return Static;
    }
    if (~RootSignatureArgumentIndex == 0)
      return {};

    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, Sampler.arg_index);
    return GetSamplerDescriptor(
        Builder, HeapPointer, Index, Sampler.range.lower_bound, DescriptorOffset,
        DescriptorCount(Builder, Sampler.arg_index)
    );
  }

  std::tuple<llvm::Value *, llvm::Value *, llvm::Value *>
  GetTextureDescriptor(
      llvm::air::AIRBuilder &AIR, llvm::Value *IntPtr, llvm::Value *Index, Texture::ResourceKind Kind, RangeId RangeId,
      uint32_t DescriptorOffset, llvm::Value *Count = nullptr
  ) {
    auto TyTextureHandle = AIR.getTextureHandleType(Kind);
    auto TyTextureDescriptor = llvm::StructType::get(
        AIR.getContext(), {TyTextureHandle, llvm::Type::getInt64Ty(AIR.getContext()),
                           llvm::Type::getInt64Ty(AIR.getContext()), llvm::Type::getInt64Ty(AIR.getContext())}
    );
    auto &B = AIR.builder;
    auto Pointer = B.CreatePointerCast(
        IntPtr, TyTextureDescriptor->getPointerTo(IntPtr->getType()->getPointerAddressSpace())
    );
    auto IdxDescriptor =
        Clamped(AIR, B.CreateAdd(B.CreateSub(Index, AIR.getInt(RangeId)), AIR.getInt(DescriptorOffset)), Count);
    return {
        B.CreateLoad(
            TyTextureHandle, //
            B.CreateGEP(TyTextureDescriptor, Pointer, {IdxDescriptor, AIR.getInt(0) /* pointer */})
        ),
        B.CreateLoad(
            llvm::Type::getInt64Ty(AIR.getContext()),
            B.CreateGEP(TyTextureDescriptor, Pointer, {IdxDescriptor, AIR.getInt(1) /* metadata */})
        ),
        B.CreateGEP(
            TyTextureDescriptor, Pointer, {IdxDescriptor, AIR.getInt(2) /* the format range (SM50_SAMPLER_BORDER) */}
        )
    };
  }

  virtual llvm::Optional<TextureDescirptor>
  GetSRVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = SRVs.find(Range);
    if (Iter == SRVs.end())
      return {};
    auto &SRV = Iter->second.first;
    if (SRV.resource_type == shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, SRV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can only be a buffer
    auto ResourceKindLogical = air::to_air_resource_type(SRV.resource_type, SRV.compared);
    auto ResourceKind = air::lowering_texture_to_array(ResourceKindLogical, SRV.compared);
    auto SampleType = std::visit(
        patterns{
            [](air::MSLInt) { return Texture::sample_int; }, [](air::MSLUint) { return Texture::sample_uint; },
            [](auto) { return Texture::sample_float; }
        },
        air::to_air_scaler_type(SRV.scaler_type)
    );
    auto MemoryAccess = SRV.sampled ? Texture::MemoryAccess::access_sample : Texture::MemoryAccess::access_read;

    auto [Handle, Metadata, FormatRange] =
        GetTextureDescriptor(Builder, HeapPointer, Index, ResourceKind, SRV.range.lower_bound, DescriptorOffset,
                             DescriptorCount(Builder, SRV.arg_index));
    return TextureDescirptor{Handle,       Metadata,   false, ResourceKind, ResourceKindLogical,
                             MemoryAccess, SampleType, FormatRange};
  }

  virtual llvm::Optional<TextureDescirptor>
  GetUAVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = UAVs.find(Range);
    if (Iter == UAVs.end())
      return {};
    auto &UAV = Iter->second.first;
    if (UAV.resource_type == shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, UAV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can only be a buffer
    auto ResourceKindLogical = air::to_air_resource_type(UAV.resource_type);
    auto ResourceKind = air::lowering_texture_to_array(ResourceKindLogical);
    auto SampleType = std::visit(
        patterns{
            [](air::MSLInt) { return Texture::sample_int; }, [](air::MSLUint) { return Texture::sample_uint; },
            [](auto) { return Texture::sample_float; }
        },
        air::to_air_scaler_type(UAV.scaler_type)
    );
    auto MemoryAccess = UAV.written
                            ? (UAV.read ? Texture::MemoryAccess::acesss_readwrite : Texture::MemoryAccess::access_write)
                            : Texture::MemoryAccess::access_read;
    auto [Handle, Metadata, FormatRange] =
        GetTextureDescriptor(Builder, HeapPointer, Index, ResourceKind, UAV.range.lower_bound, DescriptorOffset,
                             DescriptorCount(Builder, UAV.arg_index));
    return TextureDescirptor{
        Handle,  Metadata, UAV.global_coherent, ResourceKind, ResourceKindLogical, MemoryAccess, SampleType,
        FormatRange, true
    };
  }

  virtual llvm::Optional<BufferDescriptor>
  GetSRVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = SRVs.find(Range);
    if (Iter == SRVs.end())
      return {};
    auto &SRV = Iter->second.first;
    if (SRV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, SRV.arg_index);
    if (DescriptorOffset == ~0u) {
      if (auto Length = RootViewLength(Builder, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, Range))
        return BufferDescriptor{
            HeapPointer, Length, SRV.structure_stride, false, true, GetAtomicLocks(Builder), SM50_CHECK_ROOT_VIEW
        };
      return BufferDescriptor{HeapPointer, Builder.builder.getInt64(0xffffffff), SRV.structure_stride, false, false};
    }
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy()->getPointerTo(1), SRV.range.lower_bound, DescriptorOffset, true,
        DescriptorCount(Builder, SRV.arg_index)
    );

    return BufferDescriptor{
        Pointer, Metadata, SRV.structure_stride, false, !Unbounded.count({D3D12_DESCRIPTOR_RANGE_TYPE_SRV, Range}),
        nullptr, GuardCounted(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, Range)
    };
  }

  virtual llvm::Optional<BufferDescriptor>
  GetUAVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = UAVs.find(Range);
    if (Iter == UAVs.end())
      return {};
    auto &UAV = Iter->second.first;
    if (UAV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, UAV.arg_index);
    if (DescriptorOffset == ~0u) {
      if (auto Length = RootViewLength(Builder, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, Range))
        return BufferDescriptor{
            HeapPointer, Length, UAV.structure_stride, UAV.global_coherent, true, GetAtomicLocks(Builder),
            SM50_CHECK_ROOT_VIEW
        };
      return BufferDescriptor{
          HeapPointer, Builder.builder.getInt64(0xffffffff), UAV.structure_stride, UAV.global_coherent, false
      };
    }
    auto [Pointer, Metadata] = GetBufferDescriptor(
        Builder, HeapPointer, Index, Builder.getIntTy()->getPointerTo(1), UAV.range.lower_bound, DescriptorOffset, true,
        DescriptorCount(Builder, UAV.arg_index)
    );

    return BufferDescriptor{
        Pointer, Metadata, UAV.structure_stride, UAV.global_coherent,
        !Unbounded.count({D3D12_DESCRIPTOR_RANGE_TYPE_UAV, Range}), nullptr,
        GuardCounted(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, Range)
    };
  }

  virtual llvm::Optional<CounterDescriptor>
  GetUAVCounter(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) {
    if (~RootSignatureArgumentIndex == 0)
      return {};
    auto Iter = UAVs.find(Range);
    if (Iter == UAVs.end())
      return {};
    auto &UAV = Iter->second.first;
    if (UAV.resource_type != shader::common::ResourceType::NonApplicable)
      return {};
    auto DescriptorOffset = Iter->second.second;
    auto HeapPointer = GetArgument(Builder, RootSignatureArgumentIndex, UAV.arg_index);
    if (DescriptorOffset == ~0u)
      return {}; // root descriptor can not have counters associated
    auto Pointer = GetUAVCounterDescriptor(
        Builder, HeapPointer, Index, UAV.range.lower_bound, DescriptorOffset, DescriptorCount(Builder, UAV.arg_index)
    );

    return CounterDescriptor{Pointer};
  }

  llvm::Value *
  GetAtomicLocks(llvm::air::AIRBuilder &Builder) override {
    return GetArgument(Builder, RootSignatureArgumentIndex, AtomicLocksArgIndex);
  }

  llvm::Value *
  GetSparseBufferMappings(llvm::air::AIRBuilder &Builder) override {
    return SparseBufferMappings ? SparseBufferMappings
                                : Builder.builder.GetInsertBlock()->getParent()->getArg(SparseBufferMappingsIndex);
  }

  uint32_t RootSignatureArgumentIndex = ~0u;
  uint32_t StaticSamplerArgumentIndex = ~0u;
  uint32_t AtomicLocksArgIndex = ~0u;
  llvm::StructType *RootArgumentsType = nullptr;
  llvm::Value *RootArguments = nullptr;
  llvm::Value *StaticSamplers = nullptr;
  // each static sampler's anisotropy and LOD bias as its metadata has them (mullion::Sampler::createSampler)
  std::vector<std::tuple<bool, bool, bool>> StaticSamplerStates;
  uint32_t SparseBufferMappingsIndex;
  llvm::Value *SparseBufferMappings = nullptr;

  // the table ranges whose descriptors root signature 1.1 says are static, by range type (BufferDescriptor::Bounded)
  std::set<std::pair<D3D12_DESCRIPTOR_RANGE_TYPE, RangeId>> Unbounded;
  // jdrly/mullion: the static ranges whose checks SM50_SHADER_FLAG_GUARD_STATIC_RANGES alone keeps, by range type
  std::set<std::pair<D3D12_DESCRIPTOR_RANGE_TYPE, RangeId>> Guarded;

  // what a failed check of a view through a table counts as: a static range's that the guard keeps, else nothing
  std::optional<uint32_t>
  GuardCounted(D3D12_DESCRIPTOR_RANGE_TYPE Type, RangeId Range) {
    return Guarded.count({Type, Range}) ? std::optional<uint32_t>(SM50_CHECK_STATIC_RANGE) : std::nullopt;
  }
  // jdrly/mullion: a root CBV, SRV or UAV's length in the root arguments (SM50_SHADER_FLAG_CHECK_ROOT_RANGES); a root
  // view not in it carries none, as upstream's never do. A load past a root SRV or UAV reads its dropped value from
  // the lock table of 64-bit atomics, which every root signature binds, instead of the view's start, which may have
  // fewer bytes than one load: a root view keeps its own address, however few bytes its buffer has left
  std::map<std::pair<D3D12_DESCRIPTOR_RANGE_TYPE, RangeId>, uint32_t> RootViewLengths;

  // the length a root view of `Type` is checked against, or null
  llvm::Value *
  RootViewLength(llvm::air::AIRBuilder &Builder, D3D12_DESCRIPTOR_RANGE_TYPE Type, RangeId Range) {
    auto Length = RootViewLengths.find({Type, Range});
    return Length == RootViewLengths.end() ? nullptr : GetArgument(Builder, RootSignatureArgumentIndex, Length->second);
  }

  std::map<RangeId, std::pair<ConstantBufferInfo, uint64_t>> ConstantBuffers;
  std::map<RangeId, std::pair<SamplerInfo, uint64_t>> Samplers;
  std::map<RangeId, std::pair<ShaderResourceViewInfo, uint64_t>> SRVs;
  std::map<RangeId, std::pair<UnorderedAccessViewInfo, uint64_t>> UAVs;
};

std::unique_ptr<BindingMap>
setup_binding_rootsig(
    const ShaderInfo *shader_info, air::FunctionSignatureBuilder &func_signature, llvm::Module &module,
    microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE shader_type, const void *bytecode, size_t bytecode_length,
    SM50_SHADER_FLAG shader_flags, uint32_t root_sig_slot, uint32_t static_sampler_slot, bool local
) {
  using namespace microsoft;
  using namespace mullion::dxbc;
  using namespace mullion::air;
  using namespace mullion::shader::common;

  auto binding_map = std::make_unique<RootSignatureBindingMap>();
  binding_map->SparseBufferMappingsIndex = func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = kSparseBufferArgumentIndex,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = air::msl_ulong,
      .arg_name = "sparse_buffers",
      .raster_order_group = {},
  });

  const void *pRawRootSig;
  UINT RawRootSigSize;
  HRESULT hr = microsoft::DXBCGetRootSignature(bytecode, &pRawRootSig, &RawRootSigSize);
  if (FAILED(hr))
    return {};

  RootSignatureDeserializer deserializer;
  hr = deserializer.Deserialize(pRawRootSig, RawRootSigSize);
  if (FAILED(hr))
    return {};

  auto &root_sig = deserializer.desc_1_1_.Desc_1_1;

  air::ArgumentBufferBuilder builder;
  unsigned inc_attribute_index = 0;
  // root constants: their first field and their DWORDs
  std::vector<std::pair<uint32_t, uint32_t>> constants_fields;

  auto check_visibility = [=](D3D12_SHADER_VISIBILITY visibility) -> bool {
    if (visibility == D3D12_SHADER_VISIBILITY_ALL)
      return true;
    switch (visibility) {
    case D3D12_SHADER_VISIBILITY_VERTEX:
      if (shader_type == D3D10_SB_VERTEX_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_HULL:
      if (shader_type == D3D11_SB_HULL_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_DOMAIN:
      if (shader_type == D3D11_SB_DOMAIN_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_GEOMETRY:
      if (shader_type == D3D10_SB_GEOMETRY_SHADER)
        return true;
      break;
    case D3D12_SHADER_VISIBILITY_PIXEL:
      if (shader_type == D3D10_SB_PIXEL_SHADER)
        return true;
      break;
    default:
      break;
    }
    // D3D12_SHADER_VISIBILITY_AMPLIFICATION and _MESH, which the bundled header does not have
    if (visibility == D3D12_SHADER_VISIBILITY(6))
      return shader_type == D3D12_SB_AMPLIFICATION_SHADER;
    if (visibility == D3D12_SHADER_VISIBILITY(7))
      return shader_type == D3D12_SB_MESH_SHADER;
    return false;
  };

  auto range_contained = [=](const ResourceRange &range, const D3D12_DESCRIPTOR_RANGE1 &range_rootsig) -> bool {
    if (range.lower_bound < range_rootsig.BaseShaderRegister)
      return false;
    if (range_rootsig.NumDescriptors == ~0u)
      return true;
    if (range.size == ~0u)
      return true;
    if (range.lower_bound + range.size <= range_rootsig.BaseShaderRegister + range_rootsig.NumDescriptors)
      return true;
    return false;
  };

  // jdrly/mullion: each root CBV, SRV and UAV parameter, in order, with the ranges this stage binds to it
  std::vector<std::vector<std::pair<D3D12_DESCRIPTOR_RANGE_TYPE, RangeId>>> root_views;
  // jdrly/mullion: each descriptor table parameter's argument, in order, and the directly indexed heaps' (~0u: none)
  std::vector<uint32_t> tables;
  uint32_t resource_heap_arg = ~0u, sampler_heap_arg = ~0u;
  for (unsigned i = 0; i < root_sig.NumParameters; i++) {
    auto &Parameter = root_sig.pParameters[i];

    switch (Parameter.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
      auto &table = Parameter.DescriptorTable;
      unsigned descriptor_offset = 0;
      auto table_index = builder.Size();
      tables.push_back(table_index);
      std::optional<uint32_t> raster_order_group;
      for (unsigned i = 0; check_visibility(Parameter.ShaderVisibility) && i < table.NumDescriptorRanges; i++) {
        auto &range = table.pDescriptorRanges[i];
        auto calculated_descriptor_offset = range.OffsetInDescriptorsFromTableStart == ~0u
                                                ? descriptor_offset
                                                : range.OffsetInDescriptorsFromTableStart;
        descriptor_offset = calculated_descriptor_offset + range.NumDescriptors;
        // D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS, which the bundled header lacks
        constexpr uint32_t kKeepsBoundsChecks = 0x10000;
        bool bounded = uint32_t(range.Flags) & (D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | kKeepsBoundsChecks);
        // jdrly/mullion: MULLION_D3D12_GUARD_STATIC_RANGES=1 keeps a static range's checks too, and counts where they fail
        bool guarded = !bounded && (shader_flags & SM50_SHADER_FLAG_GUARD_STATIC_RANGES);
        bounded = bounded || guarded;
        auto bind = [&](RangeId id) {
          if (bounded)
            binding_map->Unbounded.erase({range.RangeType, id});
          else
            binding_map->Unbounded.insert({range.RangeType, id});
          if (guarded)
            binding_map->Guarded.insert({range.RangeType, id});
          else
            binding_map->Guarded.erase({range.RangeType, id});
        };
        switch (range.RangeType) {
        case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: {
          for (auto &[_, srv] : shader_info->srvMap) {
            if (srv.range.space != range.RegisterSpace)
              continue;
            if (range_contained(srv.range, range)) {
              auto range_id = srv.range.range_id;
              binding_map->SRVs[range_id] = {
                  srv, calculated_descriptor_offset + (srv.range.lower_bound - range.BaseShaderRegister)
              };
              binding_map->SRVs[range_id].first.arg_index = table_index;
              bind(range_id);
            }
          }
          break;
        }
        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
          for (auto &[_, uav] : shader_info->uavMap) {
            if (uav.range.space != range.RegisterSpace)
              continue;
            if (range_contained(uav.range, range)) {
              auto range_id = uav.range.range_id;
              binding_map->UAVs[range_id] = {
                  uav, calculated_descriptor_offset + (uav.range.lower_bound - range.BaseShaderRegister)
              };
              binding_map->UAVs[range_id].first.arg_index = table_index;
              bind(range_id);
              if (uav.rasterizer_order)
                raster_order_group = 1;
            }
          }
          break;
        case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
          for (auto &[_, cbuf] : shader_info->cbufferMap) {
            if (cbuf.range.space != range.RegisterSpace)
              continue;
            if (range_contained(cbuf.range, range)) {
              auto range_id = cbuf.range.range_id;
              binding_map->ConstantBuffers[range_id] = {
                  cbuf, calculated_descriptor_offset + (cbuf.range.lower_bound - range.BaseShaderRegister)
              };
              binding_map->ConstantBuffers[range_id].first.arg_index = table_index;
              bind(range_id);
            }
          }
          break;
        case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
          for (auto &[_, sampler] : shader_info->samplerMap) {
            if (sampler.range.space != range.RegisterSpace)
              continue;
            if (range_contained(sampler.range, range)) {
              auto range_id = sampler.range.range_id;
              binding_map->Samplers[range_id] = {
                  sampler, calculated_descriptor_offset + (sampler.range.lower_bound - range.BaseShaderRegister)
              };
              binding_map->Samplers[range_id].first.arg_index = table_index;
            }
          }
          break;
        default:
          assert(0 && "unknown range type");
          return {};
        }
      }
      // ordered descriptor reads precede the UAV accesses that depend on them (MSL 5.2.1.2).
      builder.DefineBuffer(
          "table" + std::to_string(i), raster_order_group ? AddressSpace::device : AddressSpace::constant,
          MemoryAccess::read, msl_int, inc_attribute_index++, raster_order_group
      );
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
      auto &constants = Parameter.Constants;
      auto qwords = (constants.Num32BitValues >> 1) + (constants.Num32BitValues & 1);
      auto first = builder.DefineInteger64("root_constant" + std::to_string(i), inc_attribute_index++);
      for (unsigned x = 1; x < qwords; x++) {
        builder.DefineInteger64("root_constant" + std::to_string(i) + "_" + std::to_string(x), inc_attribute_index++);
      }
      constants_fields.push_back({first, constants.Num32BitValues});
      if (!check_visibility(Parameter.ShaderVisibility))
        break;
      for (auto &[_, cbuf] : shader_info->cbufferMap) {
        if (cbuf.range.lower_bound == constants.ShaderRegister && cbuf.range.space == constants.RegisterSpace) {
          auto range_id = cbuf.range.range_id;
          binding_map->ConstantBuffers[range_id] = {cbuf, (~0u - 1)};
          binding_map->ConstantBuffers[range_id].first.arg_index = first;
        }
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_CBV: {
      auto &descriptor = Parameter.Descriptor;
      auto descriptor_index = builder.DefineBuffer(
          "cb" + std::to_string(i), AddressSpace::constant, MemoryAccess::read, msl_uint4, inc_attribute_index++
      );
      root_views.push_back({});
      if (!check_visibility(Parameter.ShaderVisibility))
        break;
      for (auto &[_, cbuf] : shader_info->cbufferMap) {
        if (cbuf.range.lower_bound == descriptor.ShaderRegister && cbuf.range.space == descriptor.RegisterSpace) {
          auto range_id = cbuf.range.range_id;
          binding_map->ConstantBuffers[range_id] = {cbuf, ~0u};
          binding_map->ConstantBuffers[range_id].first.arg_index = descriptor_index;
          root_views.back().push_back({D3D12_DESCRIPTOR_RANGE_TYPE_CBV, range_id});
        }
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_SRV: {
      auto &descriptor = Parameter.Descriptor;
      auto descriptor_index = builder.DefineBuffer(
          "t" + std::to_string(i), AddressSpace::device, MemoryAccess::read, msl_uint, inc_attribute_index++
      );
      root_views.push_back({});
      if (!check_visibility(Parameter.ShaderVisibility))
        break;
      for (auto &[_, srv] : shader_info->srvMap) {
        if (srv.range.lower_bound == descriptor.ShaderRegister && srv.range.space == descriptor.RegisterSpace) {
          assert(srv.resource_type == shader::common::ResourceType::NonApplicable);
          auto range_id = srv.range.range_id;
          binding_map->SRVs[range_id] = {srv, ~0u};
          binding_map->SRVs[range_id].first.arg_index = descriptor_index;
          root_views.back().push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SRV, range_id});
        }
      }
      break;
    }
    case D3D12_ROOT_PARAMETER_TYPE_UAV: {
      auto &descriptor = Parameter.Descriptor;
      auto descriptor_index = builder.Size();
      std::optional<uint32_t> raster_order_group;
      root_views.push_back({});
      if (check_visibility(Parameter.ShaderVisibility))
        for (auto &[_, uav] : shader_info->uavMap) {
          if (uav.range.lower_bound == descriptor.ShaderRegister && uav.range.space == descriptor.RegisterSpace) {
            assert(uav.resource_type == shader::common::ResourceType::NonApplicable);
            auto range_id = uav.range.range_id;
            binding_map->UAVs[range_id] = {uav, ~0u};
            binding_map->UAVs[range_id].first.arg_index = descriptor_index;
            root_views.back().push_back({D3D12_DESCRIPTOR_RANGE_TYPE_UAV, range_id});
            raster_order_group = uav.rasterizer_order ? std::optional(1) : std::nullopt;
          }
        }
      builder.DefineBuffer(
          "u" + std::to_string(i), AddressSpace::device, MemoryAccess::read_write /* FIXME */, msl_uint,
          inc_attribute_index++, raster_order_group
      );
      break;
    }
    }
  }

  uint32_t parameter_fields = builder.Size();
  // directly indexed heaps, declared after the parameters as the root signature lays them out (ResourceHeapQword,
  // SamplerHeapQword). a heap-space range (SM 6.6 createHandleFromHeap) covers its heap from descriptor 0
  auto bind_heap = [&](auto &binding, auto &ranges, uint32_t heap_index) {
    for (auto &[_, info] : ranges)
      if (info.range.space == kDescriptorHeapSpace) {
        binding[info.range.range_id] = {info, 0};
        binding[info.range.range_id].first.arg_index = heap_index;
      }
  };
  // D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED and _SAMPLER_HEAP_DIRECTLY_INDEXED, which the native
  // headers' enumeration stops before
  constexpr uint32_t kResourceHeapDirectlyIndexed = 0x400, kSamplerHeapDirectlyIndexed = 0x800;
  if (root_sig.Flags & kResourceHeapDirectlyIndexed) {
    auto ordered = std::any_of(shader_info->uavMap.begin(), shader_info->uavMap.end(), [&](auto &entry) {
      return entry.second.range.space == kDescriptorHeapSpace && entry.second.rasterizer_order;
    });
    auto heap_index = builder.DefineBuffer(
        "resource_heap", ordered ? AddressSpace::device : AddressSpace::constant, MemoryAccess::read, msl_int,
        inc_attribute_index++, ordered ? std::optional(1) : std::nullopt
    );
    resource_heap_arg = heap_index;
    bind_heap(binding_map->SRVs, shader_info->srvMap, heap_index);
    bind_heap(binding_map->UAVs, shader_info->uavMap, heap_index);
    bind_heap(binding_map->ConstantBuffers, shader_info->cbufferMap, heap_index);
  }
  if (root_sig.Flags & kSamplerHeapDirectlyIndexed) {
    sampler_heap_arg =
        builder.DefineBuffer("sampler_heap", AddressSpace::constant, MemoryAccess::read, msl_int, inc_attribute_index++);
    bind_heap(binding_map->Samplers, shader_info->samplerMap, sampler_heap_arg);
  }
  // then the lock table of 64-bit atomics (AtomicLocksQword)
  binding_map->AtomicLocksArgIndex =
      builder.DefineBuffer("atomic_locks", AddressSpace::device, MemoryAccess::read_write, msl_uint, inc_attribute_index++);
  // jdrly/mullion: then each root view's length, every root CBV, SRV and UAV parameter's in order, seen by this stage or
  // not, as the root signature lays them out (RootLengthQwords). a shader record's arguments have none
  if ((shader_flags & SM50_SHADER_FLAG_CHECK_ROOT_RANGES) && !local)
    for (unsigned i = 0; i < root_views.size(); i++) {
      auto length = builder.DefineInteger64("root_view_length" + std::to_string(i), inc_attribute_index++);
      for (auto &view : root_views[i])
        binding_map->RootViewLengths[view] = length;
    }
  // jdrly/mullion: then each descriptor table's count, every table parameter's in order, seen by this stage or not, and
  // the directly indexed heaps', resource then sampler, as the root signature lays them out (TableCountQwords)
  if ((shader_flags & SM50_SHADER_FLAG_CHECK_DESCRIPTOR_INDICES) && !local) {
    for (unsigned i = 0; i < tables.size(); i++)
      binding_map->DescriptorCounts[tables[i]] =
          builder.DefineInteger64("table_count" + std::to_string(i), inc_attribute_index++);
    for (auto [heap, name] : {std::pair{resource_heap_arg, "resource_heap_count"},
                              std::pair{sampler_heap_arg, "sampler_heap_count"}})
      if (heap != ~0u)
        binding_map->DescriptorCounts[heap] = builder.DefineInteger64(name, inc_attribute_index++);
  }

  auto [type, metadata] = builder.Build(module.getContext(), module.getDataLayout());
  binding_map->RootArgumentsType = type;
  if (local) {
    // a shader record's local root arguments: each aligned to its own size, in the parameters' order, so that root
    // constants are DWORDs with no padding of their own (DXR spec, "Shader table memory initialization"), where the
    // global block gives them whole qwords. the record has only the parameters' arguments
    std::vector<llvm::Type *> fields;
    std::vector<uint32_t> moved(type->getNumElements());
    auto constants = constants_fields.begin();
    for (uint32_t i = 0; i < parameter_fields;) {
      moved[i] = fields.size();
      if (constants != constants_fields.end() && constants->first == i) {
        fields.insert(fields.end(), constants->second, llvm::Type::getInt32Ty(module.getContext()));
        i += (constants->second + 1) / 2;
        ++constants;
      } else {
        fields.push_back(type->getElementType(i++));
      }
    }
    binding_map->RootArgumentsType = llvm::StructType::get(module.getContext(), fields);
    auto move = [&](auto &ranges) {
      std::erase_if(ranges, [&](auto &range) { return range.second.first.arg_index >= parameter_fields; });
      for (auto &[_, range] : ranges)
        range.first.arg_index = moved[range.first.arg_index];
    };
    move(binding_map->ConstantBuffers);
    move(binding_map->Samplers);
    move(binding_map->SRVs);
    move(binding_map->UAVs);
    binding_map->RootSignatureArgumentIndex = 0;
  } else {
    binding_map->RootSignatureArgumentIndex = func_signature.DefineInput(air::ArgumentBindingIndirectBuffer{
        .location_index = root_sig_slot, // SM50_BINDING_INDEX_ROOT_ARGUMENTS
        .array_size = 1,
        .memory_access = air::MemoryAccess::read,
        .address_space = air::AddressSpace::constant,
        .struct_type = type,
        .struct_type_info = metadata,
        .arg_name = "rootsig",
    });
  }

  for (unsigned i = 0; i < root_sig.NumStaticSamplers; i++) {
    auto &State = root_sig.pStaticSamplers[i];
    binding_map->StaticSamplerStates.emplace_back(
        D3D12_DECODE_IS_ANISOTROPIC_FILTER(State.Filter) && State.MaxAnisotropy > 1,
        std::lround(std::clamp(std::ldexp(State.MipLODBias, SM50_SAMPLER_METADATA_LOD_FRACTION_BITS), (float)INT16_MIN,
                               (float)INT16_MAX)) != 0,
        D3D12_DECODE_FILTER_REDUCTION(State.Filter) == D3D12_FILTER_REDUCTION_TYPE_MINIMUM ||
            D3D12_DECODE_FILTER_REDUCTION(State.Filter) == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM
    );

    for (auto &[_, sampler] : shader_info->samplerMap) {
      if (sampler.range.space != State.RegisterSpace)
        continue;
      if (sampler.range.lower_bound != State.ShaderRegister)
        continue;
      if (!check_visibility(State.ShaderVisibility))
        break;
      auto range_id = sampler.range.range_id;
      binding_map->Samplers[range_id] = {sampler, ~0u};
      binding_map->Samplers[range_id].first.arg_index = i;
    }
  }

  // ordinary stages need every resource covered; ray shaders share global and local maps (Microsoft Learn,
  // Creating a Root Signature, "Root Signature in Pipeline State Objects")
  if (shader_type != kShaderLibrary &&
      (binding_map->ConstantBuffers.size() != shader_info->cbufferMap.size() ||
       binding_map->Samplers.size() != shader_info->samplerMap.size() ||
       binding_map->SRVs.size() != shader_info->srvMap.size() || binding_map->UAVs.size() != shader_info->uavMap.size()))
    return {};
  if (root_sig.NumStaticSamplers == 0)
    return binding_map;

  // a shader record has no static samplers: its shader is told where they are (bind_rootsig_arguments), which
  // stands for an argument after the record
  binding_map->StaticSamplerArgumentIndex =
      local ? binding_map->RootSignatureArgumentIndex + 1
            : func_signature.DefineInput(air::ArgumentBindingBuffer{
                  .buffer_size = {},
                  .location_index = static_sampler_slot, // SM50_BINDING_INDEX_STATIC_SAMPLERS
                  .array_size = 0,
                  .memory_access = air::MemoryAccess::read,
                  .address_space = air::AddressSpace::constant,
                  .type = air::msl_ulong,
                  .arg_name = "static_samplers",
                  .raster_order_group = {},
              });

  return binding_map;
}

void
bind_rootsig_arguments(
    BindingMap &map, llvm::IRBuilder<> &builder, llvm::Value *arguments, llvm::Value *static_samplers,
    llvm::Value *sparse_buffers
) {
  auto &rootsig = static_cast<RootSignatureBindingMap &>(map);
  rootsig.RootArguments = builder.CreatePointerCast(
      arguments, rootsig.RootArgumentsType->getPointerTo(arguments->getType()->getPointerAddressSpace())
  );
  rootsig.StaticSamplers = static_samplers;
  rootsig.SparseBufferMappings = sparse_buffers;
}

class CombinedBindingMap : public BindingMap {
public:
  std::unique_ptr<BindingMap> maps[2];

  template <typename Get>
  auto
  First(Get &&get) -> decltype(get(*maps[0])) {
    for (auto &map : maps)
      if (map)
        if (auto found = get(*map))
          return found;
    return {};
  }

  llvm::Optional<ConstantBufferDescriptor>
  GetConstantBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetConstantBuffer(Builder, Range, Index); });
  }
  llvm::Optional<SamplerDescriptor>
  GetSampler(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetSampler(Builder, Range, Index); });
  }
  llvm::Optional<TextureDescirptor>
  GetSRVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetSRVTexture(Builder, Range, Index); });
  }
  llvm::Optional<TextureDescirptor>
  GetUAVTexture(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetUAVTexture(Builder, Range, Index); });
  }
  llvm::Optional<BufferDescriptor>
  GetSRVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetSRVBuffer(Builder, Range, Index); });
  }
  llvm::Optional<BufferDescriptor>
  GetUAVBuffer(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetUAVBuffer(Builder, Range, Index); });
  }
  llvm::Optional<CounterDescriptor>
  GetUAVCounter(llvm::air::AIRBuilder &Builder, RangeId Range, llvm::Value *Index) override {
    return First([&](BindingMap &map) { return map.GetUAVCounter(Builder, Range, Index); });
  }
  // the device's lock table comes with the global root arguments, which are the second map
  llvm::Value *
  GetAtomicLocks(llvm::air::AIRBuilder &Builder) override {
    return maps[1] ? maps[1]->GetAtomicLocks(Builder) : nullptr;
  }
  llvm::Value *
  GetSparseBufferMappings(llvm::air::AIRBuilder &Builder) override {
    return First([&](BindingMap &map) { return map.GetSparseBufferMappings(Builder); });
  }
};

std::unique_ptr<BindingMap>
combine_bindings(std::unique_ptr<BindingMap> first, std::unique_ptr<BindingMap> second) {
  auto combined = std::make_unique<CombinedBindingMap>();
  combined->maps[0] = std::move(first);
  combined->maps[1] = std::move(second);
  return combined;
}

} // namespace mullion::dxbc
