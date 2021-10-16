
"""Parses a V8 profiling log and emits a list of the hot functions.
"""

import re
import sys

filename = sys.argv[1]
top_threshold = float(sys.argv[2])
in_parent_threshold = float(sys.argv[3])

print(f'parsing {filename} with top_threshold {top_threshold}% and in_parent_threshold {in_parent_threshold}')
print()

print('[')

in_js = False
in_bottom_up = False

found_sections = 0

# A stack of the percentages we've seen so far in each depth.
bottom_up_stack = []

for line in open(sys.argv[1]).readlines():
    if ']:' in line:
        in_js = False
        if '[JavaScript]' in line:
            in_js = True
            found_sections += 1
        elif '[Bottom up (heavy) profile]' in line:
            in_bottom_up = True
            found_sections += 1
    elif in_js:
        continue
        if ' ticks ' in line:
            # This is the header.
            continue

        # The JS section looks like this:
        #    N    X%    Y%  Function: *name
        # where N is the # of ticks, X is the total %, Y is the nonlib %, and
        # name is the function name.
        m = re.match('\s*(\d+)\s+([\d.]+)%\s+([\d.]+)%\s+Function: [*]([\w_.$@<>|]+)\s*', line.strip())
        if not m:
            continue
        total = m[2]
        name = m[4]
        if float(total) >= threshold:
            print(f'  "{name}",')

    elif in_bottom_up:
        if ' ticks ' in line:
            # This is the header.
            continue

        print()
        # The bottom-up section looks like this:
        #    N    X%   Function: *name
        # where N is the # of ticks, X is the % in the parent, and name is the
        # function name. Note that *name may also be UNKNOWN, in which case
        # Function: does not appear.
        # The whitespace after the % indicates the level of nesting.
        m = re.match('\s*(\d+)\s+([\d.]+)%(\s+)(Function: [*])?([\w_.$@<>|]+)\s*', line.strip())
        if not m:
            continue
        in_parent = float(m[2])
        indentation = m[3]
        name = m[5]
        nesting = len(indentation) // 2 - 1
        print(f'  // {in_parent}  {nesting}  {name}')
        if nesting == 0:
            # This is a top-level entry.
            if in_parent >= top_threshold:
                # Start the stack for the children, beginning with 0% of
                # children appearance seen so far.
                bottom_up_stack = [0.0]
                print(f'  "{name}",')
            else:
                # This is not something we care about. Mark the stack
                # accordingly
                bottom_up_stack = [-1.0]
        else:
            # This is a nested child.

            last_nesting = len(bottom_up_stack)

            if nesting < last_nesting:
                # This is a de-indentation. Collapse all the higher things so
                # that the stack is at the right length for us.
                bottom_up_stack = bottom_up_stack[:nesting]

            # If the most recent toplevel says we should stop, stop.
            if bottom_up_stack[-1] < 0:
                continue

            if nesting > last_nesting:
                # This is an indentation. Add a new entry at the end of the
                # stack.
                assert(nesting == last_nesting + 1)
                bottom_up_stack.append(0.0)

            print(f'  // child, pre-stack: {bottom_up_stack}')

            # Check if we are still within the range we care about.
            if bottom_up_stack[-1] <= in_parent_threshold:
                print(f'  "{name}",')
                # Add our percentage to the stack's accumulated value, which
                # will affect whether subsequent children are printed.
                bottom_up_stack[-1] += in_parent
                assert(bottom_up_stack[-1] <= 101.0) # output has some rounding
            else:
                # Don't print this or the children.
                bottom_up_stack[-1] = -1.0



print(']')

assert(found_sections == 2)

