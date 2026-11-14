// jdrly/mullion: the converter's fixes of #7 on shaders made here, without a GPU. the native airconv converts:
// - a pixel shader that declares v1 as a system value the converter doesn't handle, by dcl_input_ps_siv (a vertex id)
//   and by dcl_input_ps_sgv (an instance id), and copies v1 to its render target. the declaration is ignored: the
//   converted function takes no argument for it. before #7 the converter still copied v1 from an argument it never
//   chose (assigned_index unset), of a function that has none, so it read past the arguments or crashed;
// - the same shader with v1 a front face, which the converter handles: one argument, so the check above can see one;
// - a DXIL compute shader with a typed SRV of doubles (Buffer<double>, component type F64), which is refused with
//   "unsupported: a typed resource of doubles". before #7 it was bound with a scalar type never set. the same shader
//   with floats converts, so the refusal is the doubles', not the fixture's.
//
//   host_converter_fixes <airconv>
//
// the pixel shaders are made token by token, and the compute shader is LLVM assembly kept here, made bitcode by the
// LLVM the converter is built with, which is how the converter reads DXIL: no compiler or binary fixture is needed.
#include "airconv_host.hpp"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <unistd.h>

// a signature part (ISGN, OSGN): its elements, each a semantic, its index, system value (D3D_NAME), component type
// (D3D_REGISTER_COMPONENT_TYPE), register and mask, then their names
struct Element {
  const char *name;
  uint32_t index, system_value, component_type, reg, mask;
};

static Words
signature(std::initializer_list<Element> elements) {
  Words words{uint32_t(elements.size()), 8};
  std::string names;
  for (auto &e : elements) {
    words.insert(words.end(), {uint32_t(8 + 24 * elements.size() + names.size()), e.index, e.system_value,
                               e.component_type, e.reg, e.mask | e.mask << 8});
    names += e.name;
    names += '\0';
  }
  names.resize((names.size() + 3) / 4 * 4);
  Words packed(names.size() / 4);
  memcpy(packed.data(), names.data(), names.size());
  words.insert(words.end(), packed.begin(), packed.end());
  return words;
}

// ps_5_0:
//   dcl_input_ps_siv constant v1.x, <name>    (or dcl_input_ps_sgv v1.x, <name>)
//   dcl_output o0.xyzw
//   mov o0.xyzw, v1.xxxx
//   ret
static Words
pixel_program(uint32_t declaration, uint32_t name) {
  constexpr uint32_t x = D3D10_SB_OPERAND_4_COMPONENT_MASK_X, all = D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL;
  Words p{ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(D3D10_SB_PIXEL_SHADER, 5, 0), 0};
  Words input = masked(D3D10_SB_OPERAND_TYPE_INPUT, 1, x);
  input.push_back(ENCODE_D3D10_SB_NAME(name));
  instruction(p, declaration, {input},
              declaration == D3D10_SB_OPCODE_DCL_INPUT_PS_SIV
                  ? ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(D3D10_SB_INTERPOLATION_CONSTANT)
                  : 0);
  instruction(p, D3D10_SB_OPCODE_DCL_OUTPUT, {masked(D3D10_SB_OPERAND_TYPE_OUTPUT, 0, all)});
  instruction(p, D3D10_SB_OPCODE_MOV,
              {masked(D3D10_SB_OPERAND_TYPE_OUTPUT, 0, all),
               swizzled(D3D10_SB_OPERAND_TYPE_INPUT, 1, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEX)});
  instruction(p, D3D10_SB_OPCODE_RET);
  p[1] = p.size();
  return p;
}

// cs_6_0, as DXC writes `Buffer<ELEMENT> b : register(t0); [numthreads(1, 1, 1)] void main() {}`: one typed SRV,
// its element's component type (DxilConstants.h ComponentType: 9 F32, 10 F64) the tag 0 of its properties
static const char *compute_ir = R"(
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%"class.Buffer" = type { ELEMENT }

@b = external constant %"class.Buffer"

define void @main() {
  ret void
}

!dx.version = !{!0}
!dx.shaderModel = !{!1}
!dx.resources = !{!2}
!dx.entryPoints = !{!6}

!0 = !{i32 1, i32 0}
!1 = !{!"cs", i32 6, i32 0}
!2 = !{!3, null, null, null}
!3 = !{!4}
!4 = !{i32 0, %"class.Buffer"* @b, !"b", i32 0, i32 0, i32 1, i32 10, i32 0, !5}
!5 = !{i32 0, i32 COMPONENT}
!6 = !{void ()* @main, !"main", null, !2, !7}
!7 = !{i32 4, !8}
!8 = !{i32 1, i32 1, i32 1}
)";

// the DXIL part of `ir`: a DxilProgramHeader (the shader's kind and model, its size in words), then a
// DxilBitcodeHeader ("DXIL", DXIL 1.0, where its bitcode starts from it, its size), then the bitcode
static Words
dxil(std::string ir, const char *element, uint32_t component) {
  for (auto [from, to] :
       {std::pair<std::string, std::string>{"ELEMENT", element}, {"COMPONENT", std::to_string(component)}})
    ir.replace(ir.find(from), from.size(), to);
  llvm::LLVMContext context;
  context.setOpaquePointers(false); // DXIL's pointers are typed, as the converter reads them
  llvm::SMDiagnostic error;
  auto module = llvm::parseAssemblyString(ir, error, context);
  if (!module) {
    error.print("host_converter_fixes", llvm::errs());
    return {};
  }
  std::string bitcode;
  llvm::raw_string_ostream out(bitcode);
  llvm::WriteBitcodeToFile(*module, out);
  out.flush();
  bitcode.resize((bitcode.size() + 3) / 4 * 4);
  constexpr uint32_t magic = 0x4c495844; // "DXIL"
  Words words{uint32_t(D3D11_SB_COMPUTE_SHADER << 16 | 6 << 4), uint32_t(6 + bitcode.size() / 4), magic, 0x100, 16,
              uint32_t(bitcode.size())};
  words.resize(6 + bitcode.size() / 4);
  memcpy(words.data() + 6, bitcode.data(), bitcode.size());
  return words;
}

int
main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <airconv>\n", argv[0]);
    return 2;
  }
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / ("host_converter_fixes." + std::to_string(::getpid()));
  fs::create_directories(dir);
  auto write = [&](const std::string &name, const std::string &bytes) {
    std::ofstream(dir / name, std::ios::binary) << bytes;
  };
  // the converter's LLVM assembly of shader `name`, or what it wrote to stderr when it failed
  struct Result {
    bool converted;
    std::string text;
  };
  auto convert = [&](const std::string &name) {
    std::string command = "\"" + std::string(argv[1]) + "\" -S -o \"" + (dir / (name + ".ll")).string() + "\" \"" +
                          (dir / (name + ".dxbc")).string() + "\" 2>\"" + (dir / (name + ".err")).string() + "\"";
    int status = std::system(command.c_str());
    std::printf("%s: airconv exited with %d\n", name.c_str(), status);
    return status == 0 ? Result{true, read(dir / (name + ".ll"))} : Result{false, read(dir / (name + ".err"))};
  };
  // the parameters of the converted function, as its LLVM assembly declares them
  auto parameters = [](const Result &result) {
    auto at = result.text.find("@shader_main(");
    if (!result.converted || at == std::string::npos)
      return std::string("(not converted)");
    at += strlen("@shader_main(");
    return result.text.substr(at, result.text.find(')', at) - at);
  };

  constexpr uint32_t uint_type = 1, float_type = 3; // D3D_REGISTER_COMPONENT_UINT32, _FLOAT32
  auto pixel = [&](uint32_t declaration, uint32_t name, const char *semantic) {
    return container({{"ISGN", signature({{semantic, 0, name, uint_type, 1, 1}})},
                      {"OSGN", signature({{"SV_Target", 0, 0, float_type, 0, 15}})},
                      {"SHEX", pixel_program(declaration, name)}});
  };
  write("siv.dxbc", pixel(D3D10_SB_OPCODE_DCL_INPUT_PS_SIV, D3D10_SB_NAME_VERTEX_ID, "SV_VertexID"));
  write("sgv.dxbc", pixel(D3D10_SB_OPCODE_DCL_INPUT_PS_SGV, D3D10_SB_NAME_INSTANCE_ID, "SV_InstanceID"));
  write("front.dxbc", pixel(D3D10_SB_OPCODE_DCL_INPUT_PS_SGV, D3D10_SB_NAME_IS_FRONT_FACE, "SV_IsFrontFace"));
  for (auto [name, what] : {std::pair{"siv", "an unhandled dcl_input_ps_siv converts and adds no argument"},
                            std::pair{"sgv", "an unhandled dcl_input_ps_sgv converts and adds no argument"}}) {
    auto result = convert(name);
    std::printf("%s: shader_main(%s)\n", name, parameters(result).c_str());
    check(parameters(result).empty(), what);
  }
  auto front = convert("front");
  std::printf("front: shader_main(%s)\n", parameters(front).c_str());
  check(front.converted && !parameters(front).empty(), "a front face, which is handled, converts to an argument");

  write("float.dxbc", container({{"ISGN", {0, 8}}, {"OSGN", {0, 8}}, {"DXIL", dxil(compute_ir, "float", 9)}}));
  write("double.dxbc", container({{"ISGN", {0, 8}}, {"OSGN", {0, 8}}, {"DXIL", dxil(compute_ir, "double", 10)}}));
  check(convert("float").converted, "a DXIL typed SRV of floats converts");
  auto doubles = convert("double");
  std::printf("double: %s", doubles.converted ? "converted\n" : doubles.text.c_str());
  check(!doubles.converted && doubles.text == "unsupported: a typed resource of doubles\n",
        "a DXIL typed SRV of doubles is refused as unsupported");

  // MULLION_KEEP_OUTPUT: the outputs stay, for a pull request to attach
  if (std::getenv("MULLION_KEEP_OUTPUT"))
    std::printf("outputs in %s\n", dir.c_str());
  else
    fs::remove_all(dir);
  return failures ? 1 : 0;
}
