#!/usr/bin/env python3
"""
Generate stacktraverse.c - Stack traversal functions for libexecinfo

This script generates C code with switch statements that use GCC's
__builtin_return_address() and __builtin_frame_address() functions
to traverse the call stack up to a specified depth.

The generated functions work around the limitation that these builtins
only accept compile-time constants as arguments.

Boundary safety, for both generated functions:
  __builtin_frame_address(N) for N > 0 walks N saved-frame-pointer
  links starting at the current frame. On the common case of a
  standard-ABI stack with frame pointers enabled, the outermost real
  frame's saved-FP slot holds 0 by convention, so frame_address(depth+1)
  resolves to NULL via one ordinary (non-faulting) memory read rather
  than crashing. frame_address(depth+2) and beyond would require
  dereferencing through that NULL, which does fault -- so callers must
  never query more than one level past the last level that returned
  non-NULL. backtrace() in execinfo.c respects this with two stop
  conditions: it stops the moment getreturnaddr() returns NULL, and it
  additionally checks each level's frame address (via getframeaddr())
  before descending -- a link that fails to advance strictly upward
  (broken chains left by FP-less libc frames hold garbage like 0x1)
  terminates the walk before anything dereferences it.

  getreturnaddr() additionally guards with `if (!fp) return NULL`
  before calling __builtin_return_address() at the same level. This
  guard is NOT a generic depth check (frame_address() already produces
  the correct NULL at the boundary on its own) -- it exists because
  __builtin_return_address() is a *different* builtin with its own,
  separately-documented requirement that the frame at that level
  actually exist, independent of whatever frame_address() returned for
  it. getframeaddr() makes no such second call, so it doesn't need an
  equivalent guard: returning frame_address()'s result directly is
  already correct at the boundary.
"""

import argparse
import sys
from typing import TextIO

PROLOGUE = '''#include <stddef.h>

#include "stacktraverse.h"

void *
get{name}addr(int level)
{{
    switch(level) {{'''

# +1 below: skip the get{return,frame}addr() function's own stack frame
# so that level 0, as seen by the caller of backtrace(), means
# "the frame that called backtrace()" rather than "backtrace() itself".
BODY_RETURN = '''    case {case}: {{
        void *fp = __builtin_frame_address({addr});
        if (!fp) return NULL;
        return __builtin_return_address({addr});
    }}'''
BODY_FRAME = '    case {case}: return __builtin_frame_address({addr});'

EPILOGUE = '''    default: return NULL;
    }
}'''

DEFAULT_MAXDEPTH = 128

def generate_function(name: str, maxnum: int, output: TextIO) -> None:
    """
    Generate a single stack traversal function.
    Args:
        name: Function type ('return' or 'frame')
        maxnum: Maximum stack depth to support
        output: Output file handle
    """
    print(PROLOGUE.format(name=name), file=output)
    for i in range(maxnum):
        if name == "return":
            print(BODY_RETURN.format(case=i, addr=i + 1), file=output)
        else:
            print(BODY_FRAME.format(case=i, addr=i + 1), file=output)
    print(EPILOGUE, file=output)

def main() -> int:
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description='Generate stacktraverse.c for libexecinfo',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )

    parser.add_argument(
        '--max-depth', '-d',
        type=int,
        default=DEFAULT_MAXDEPTH,
        help='Maximum stack traversal depth'
    )

    parser.add_argument(
        '--output', '-o',
        type=argparse.FileType('w'),
        default=sys.stdout,
        help='Output file (default: stdout)'
    )

    args = parser.parse_args()

    # Validate depth
    if args.max_depth <= 0:
        print("Error: max-depth must be positive", file=sys.stderr)
        return 1

    if args.max_depth > 1024:
        print("Warning: Large max-depth may cause compilation issues",
              file=sys.stderr)

    try:
        # Generate both functions
        generate_function("return", args.max_depth, args.output)
        print(file=args.output)  # Empty line separator
        generate_function("frame", args.max_depth, args.output)

        if args.output != sys.stdout:
            print(f"Generated stacktraverse.c with depth {args.max_depth}")

        return 0

    except Exception as e:
        print(f"Error generating code: {e}", file=sys.stderr)
        return 1

    finally:
        if args.output != sys.stdout:
            args.output.close()

if __name__ == '__main__':
    sys.exit(main())
