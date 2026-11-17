#!/usr/bin/env python3
"""jdrly/mullion: mullion::condition_variable, the Win32 one of src/util/thread.hpp, copied as it is for
host_condition_variable to compile against a scripted SleepConditionVariableSRW. it begins at the class and ends at
its closing brace; a class missing, found twice or without its four waits fails the build, so the test never runs on
a stale copy.

  condition_variable.py <thread.hpp> <out .inc>
"""
import sys
from pathlib import Path

BEGIN = "class condition_variable {"
END = "};"
WAITS = ["std::cv_status wait_for(", "bool wait_for(", "wait_until(std::unique_lock<mullion::mutex> &lock,",
         "bool wait_until("]


def main(source, out):
    lines = Path(source).read_text().splitlines()
    begins = [i for i, line in enumerate(lines) if line.strip() == BEGIN]
    if len(begins) != 1:
        sys.exit(f"{source}: {len(begins)} '{BEGIN}', want 1")
    ends = [i for i, line in enumerate(lines) if i > begins[0] and line == END]
    if not ends:
        sys.exit(f"{source}: no '{END}' at the start of a line after '{BEGIN}'")
    block = lines[begins[0]:ends[0] + 1]
    for wait in WAITS:
        found = sum(line.strip().startswith(wait) for line in block)
        if found != 1:
            sys.exit(f"{source}: the class has {found} '{wait}', want 1")
    Path(out).write_text(f"#line {begins[0] + 1} \"{Path(source).name}\"\n" + "\n".join(block) + "\n")


if __name__ == "__main__":
    main(*sys.argv[1:])
