#!/usr/bin/env python3
"""jdrly/mullion: the loop of CompileFunction (src/d3d12/d3d12_pipeline_graphics.cpp) that hashes a shader's compile
arguments into the shader cache's key, copied as it is for host_compile_key to compile. it begins at the loop and ends
where the key is taken; either missing fails the build, so the test never runs on a stale copy.

  compile_key.py <d3d12_pipeline_graphics.cpp> <out .inc>
"""
import sys
from pathlib import Path

BEGIN = "for (auto arg = arguments; arg; arg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next) {"
END = "auto key = h.final();"


def main(source, out):
    lines = Path(source).read_text().splitlines()
    begins = [i for i, line in enumerate(lines) if line.strip() == BEGIN]
    if len(begins) != 1:
        sys.exit(f"{source}: {len(begins)} argument loops, want 1")
    ends = [i for i, line in enumerate(lines) if i > begins[0] and line.strip() == END]
    if not ends:
        sys.exit(f"{source}: no '{END}' after the argument loop")
    block = lines[begins[0]:ends[0]]
    Path(out).write_text(f"#line {begins[0] + 1} \"{Path(source).name}\"\n" + "\n".join(block) + "\n")


if __name__ == "__main__":
    main(*sys.argv[1:])
