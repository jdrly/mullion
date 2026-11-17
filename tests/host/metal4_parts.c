// jdrly/mullion: what a Metal 4 command buffer leaves for the next ones of its queue (MULLION_METAL4_REUSE_PARTS,
// upstream's 0fedd37, private ticket 54), through the bridge's own unix calls (src/winemetal/unix/winemetal_unix.c,
// built into this program), on the machine's GPU and without Wine.
//
// A compute pass binds a visible function table at buffer slot 3 of its argument table, which only
// WMTComputeCommandSetVisibleFunctionTable writes, and nothing at slots 1 and 2. Its command buffer completes and is
// released, and so is the function table. The next command buffer of the queue takes that buffer's argument table
// from the pool, and its pass reads slot 3 without binding it. A new table holds 0 there; a pooled one must too, or
// the slot still names the released function table, which the new buffer doesn't keep resident. The kernel reads
// the slot's 64 bits and never calls through them.
//
// Three rounds of that, on, off and on again, with their controls: the kernel sees a function table bound in its own
// pass, so the reading is real; on, the second buffer has the first one's table, so the pool was taken, and the line
// says so the first time; off, no buffer has a table from the pool (which the first round left parts in) and none
// leaves its own there, so the third round takes the first one's. Exit 77 (skipped) where the machine has no Metal 4.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define WINEMETAL_API
#include "winemetal_thunks.h"

typedef int NTSTATUS;

// the runtime's weak references, which a file built without ARC reaches through these
id objc_initWeak(id *location, id value);
id objc_loadWeakRetained(id *location);
void objc_destroyWeak(id *location);

// the three of Wine's ntdll the bridge links against; nothing here reaches them
NTSTATUS
NtSetEvent(void *handle, void *prev_state) {
  abort();
}
NTSTATUS
NtDuplicateObject(
    void *source_process, void *source, void *dest_process, void **dest, uint32_t access, uint32_t attributes,
    uint32_t options
) {
  abort();
}
NTSTATUS
NtClose(void *handle) {
  abort();
}

extern const void *__wine_unix_call_funcs[];

static void
call(unsigned code, void *params) {
  NTSTATUS status = ((NTSTATUS (*)(void *))__wine_unix_call_funcs[code])(params);
  if (status) {
    printf("unix call %u: 0x%x\n", code, status);
    exit(1);
  }
}

static int failures;

static void
expect(bool ok, const char *what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok)
    failures++;
}

static NSString *const kSource = @"#include <metal_stdlib>\n"
                                  "using namespace metal;\n"
                                  "[[visible]] int vf(int x) { return x + 1; }\n"
                                  "kernel void probe(device ulong *out [[buffer(0)]], device ulong *slot [[buffer(3)]]) {\n"
                                  "  out[0] = reinterpret_cast<ulong>(slot);\n"
                                  "  out[1] = slot != nullptr ? 1 : 0;\n"
                                  "}\n";

static id<MTLDevice> device;
static id<MTLComputePipelineState> pso;
static id<MTLFunction> vf;
static id<MTL4CommandQueue> queue;
static id<MTLFence> fence;
static id<MTLSharedEvent> event;
static id<MTLResidencySet> global;
static id<MTLBuffer> out;
static uint64_t value;

// Direct3D 12's counts (d3d12_command_queue.cpp, NewCommandBuffer)
static const struct WMTMetal4BindingCounts kBindings = {31, 128, 16};

static id<MTLVisibleFunctionTable>
new_function_table(void) {
  MTLVisibleFunctionTableDescriptor *desc = [[[MTLVisibleFunctionTableDescriptor alloc] init] autorelease];
  desc.functionCount = 1;
  id<MTLVisibleFunctionTable> table = [pso newVisibleFunctionTableWithDescriptor:desc];
  [table setFunction:[pso functionHandleWithFunction:vf] atIndex:0];
  return table;
}

// the compute argument table a bridge command buffer has, read from its ivars
static id
compute_table(obj_handle_t buffer) {
  id object = (id)buffer;
  Ivar ivar = class_getInstanceVariable(object_getClass(object), "tables");
  if (!ivar)
    return nil;
  id *tables = (id *)((char *)object + ivar_getOffset(ivar));
  return tables[3]; // Metal4ComputeBinding
}

// a command buffer of one compute pass that runs the kernel with its output at `slot_out` (16 bytes each), and the
// function table bound at slot 3 when one is given; committed and completed
static obj_handle_t
run(bool reuse, id<MTLVisibleFunctionTable> table, unsigned slot_out) {
  struct unixcall_metal4_new p = {
      .queue = (obj_handle_t)queue, .fence = (obj_handle_t)fence, .event = (obj_handle_t)event,
      .residency = (obj_handle_t)global, .value = ++value, .bindings = kBindings, .reuse_parts = reuse
  };
  call(WMTUnixMetal4NewBuffer, &p);
  obj_handle_t buffer = p.ret;
  struct unixcall_metal4_encoder e = {.buffer = buffer, .kind = WMTMetal4Compute};
  call(WMTUnixMetal4Encoder, &e);
  struct wmtcmd_compute_dispatch dispatch = {.type = WMTComputeCommandDispatchThreads, .size = {1, 1, 1}};
  struct wmtcmd_compute_setvisiblefunctiontable functions = {
      .type = WMTComputeCommandSetVisibleFunctionTable, .table = (obj_handle_t)table, .index = 3
  };
  struct wmtcmd_compute_setbuffer output = {
      .type = WMTComputeCommandSetBuffer, .buffer = (obj_handle_t)out, .offset = slot_out * 16, .index = 0
  };
  struct wmtcmd_compute_setpso pipeline = {
      .type = WMTComputeCommandSetPSO, .pso = (obj_handle_t)pso, .threadgroup_size = {1, 1, 1}
  };
  WMT_MEMPTR_SET(pipeline.next, &output);
  WMT_MEMPTR_SET(output.next, table ? (void *)&functions : (void *)&dispatch);
  WMT_MEMPTR_SET(functions.next, &dispatch);
  struct unixcall_metal4_commands c = {.encoder = e.ret, .kind = WMTMetal4Compute};
  WMT_MEMPTR_SET(c.head, &pipeline);
  call(WMTUnixMetal4Commands, &c);
  struct unixcall_generic_obj_uint64_noret end = {e.ret, 0};
  call(WMTUnixMetal4EndEncoder, &end);
  call(WMTUnixMetal4Commit, &buffer);
  call(WMTUnixMetal4Wait, &buffer);
  struct unixcall_metal4_feedback f = {.buffer = buffer};
  call(WMTUnixMetal4Feedback, &f);
  if (f.ret.status != WMTCommandBufferStatusCompleted) {
    printf("command buffer status %u\n", f.ret.status);
    exit(1);
  }
  return buffer;
}

// releases the command buffer and waits until it is gone: Metal's feedback handler may hold it a little longer, and
// its dealloc is what leaves the parts in the pool
static void
release(obj_handle_t buffer) {
  id weak = nil;
  objc_initWeak(&weak, (id)buffer);
  [(id)buffer release];
  for (unsigned ms = 0;; ms++) {
    id strong = objc_loadWeakRetained(&weak);
    [strong release];
    if (!strong)
      break;
    if (ms == 5000) {
      printf("a command buffer is still alive 5 s after its release\n");
      exit(1);
    }
    usleep(1000);
  }
  objc_destroyWeak(&weak);
}

static const uint64_t *
reading(unsigned slot_out) {
  return (const uint64_t *)[out contents] + 2 * slot_out;
}

// stderr into a file while `block` runs, then the file's text, also written to stderr
static NSString *
captured(void (^block)(void)) {
  char path[] = "/tmp/metal4_parts.XXXXXX";
  int fd = mkstemp(path);
  fflush(stderr);
  int saved = dup(2);
  dup2(fd, 2);
  block();
  fflush(stderr);
  dup2(saved, 2);
  close(saved);
  close(fd);
  NSString *text = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
  unlink(path);
  fputs(text.UTF8String ?: "", stderr);
  return text ?: @"";
}

// one round: the first buffer binds a function table and is released with it, then the second reads the slot.
// `pooled`, when not nil, is the compute table the pool's last parts have; the round's two tables are returned, held
static void
round_of(bool reuse, unsigned round, id pooled, id *first_table, id *second_table) {
  const unsigned first = 2 * round, second = first + 1;
  printf("round %u: MULLION_METAL4_REUSE_PARTS=%d\n", round + 1, reuse);
  id<MTLVisibleFunctionTable> table = new_function_table();
  uint64_t id_bits = table.gpuResourceID._impl;
  __block obj_handle_t a = 0, b = 0;
  @autoreleasepool {
    a = run(reuse, table, first);
  }
  printf("     function table gpuResourceID 0x%llx, read in its own pass 0x%llx\n", (unsigned long long)id_bits,
         (unsigned long long)reading(first)[0]);
  expect(reading(first)[1] == 1, "the kernel sees the function table bound in its own pass");
  // held, so that no other table can get its address
  id held = *first_table = [compute_table(a) retain];
  expect(held != nil, "the first buffer has a compute table");
  if (pooled)
    expect((held == pooled) == reuse, reuse ? "the first buffer has the pool's last compute table"
                                            : "the first buffer has a compute table of its own, not the pool's");
  [table release];
  release(a);
  NSString *text = captured(^{
    @autoreleasepool {
      b = run(reuse, nil, second);
    }
  });
  *second_table = [compute_table(b) retain];
  expect((*second_table == held) == reuse, reuse ? "the second buffer has the first one's compute table"
                                                 : "the second buffer has a compute table of its own");
  if (!round)
    expect([text containsString:@"mullion: metal4 parts=reused\n"], "the line says the parts were reused");
  printf("     slot 3 in the second buffer's pass 0x%llx\n", (unsigned long long)reading(second)[0]);
  expect(reading(second)[0] == 0 && reading(second)[1] == 0,
         "slot 3 holds nothing in the second buffer's pass, as in a new table");
  release(b);
}

int
main(void) {
  @autoreleasepool {
    if (@available(macOS 26.0, *)) {
      device = MTLCreateSystemDefaultDevice();
      if (!device || ![device supportsFamily:MTLGPUFamilyMetal4]) {
        printf("skipped: no Metal 4 device\n");
        return 77;
      }
    } else {
      printf("skipped: macOS before 26\n");
      return 77;
    }
    printf("device %s\n", device.name.UTF8String);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:kSource options:nil error:&error];
    if (!library) {
      printf("library: %s\n", error.description.UTF8String);
      return 1;
    }
    vf = [library newFunctionWithName:@"vf"];
    MTLComputePipelineDescriptor *desc = [[[MTLComputePipelineDescriptor alloc] init] autorelease];
    desc.computeFunction = [[library newFunctionWithName:@"probe"] autorelease];
    MTLLinkedFunctions *linked = [[[MTLLinkedFunctions alloc] init] autorelease];
    linked.functions = @[ vf ];
    desc.linkedFunctions = linked;
    pso = [device newComputePipelineStateWithDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:&error];
    if (!pso) {
      printf("pipeline: %s\n", error.description.UTF8String);
      return 1;
    }
    queue = [device newMTL4CommandQueue];
    fence = [device newFence];
    event = [device newSharedEvent];
    global = [device newResidencySetWithDescriptor:[[[MTLResidencySetDescriptor alloc] init] autorelease] error:nil];
    [global commit];
    out = [device newBufferWithLength:6 * 16 options:MTLResourceStorageModeShared];
    memset([out contents], 0xab, [out length]);
    if (!queue || !fence || !event || !global || !out) {
      printf("setup failed\n");
      return 1;
    }
    // on: the second buffer takes the first one's parts, and leaves them in the pool. off: neither buffer takes
    // them, and neither leaves its own, so that on again takes the first round's
    id tables[6];
    round_of(true, 0, nil, &tables[0], &tables[1]);
    round_of(false, 1, tables[1], &tables[2], &tables[3]);
    round_of(true, 2, tables[1], &tables[4], &tables[5]);
    for (unsigned i = 0; i < 6; i++)
      [tables[i] release];
  }
  printf("%s\n", failures ? "FAILED" : "passed");
  return failures ? 1 : 0;
}
