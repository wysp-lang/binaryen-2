
"""Parses a V8 profiling log and emits a list of the hot functions.
"""

import re
import sys

filename = sys.argv[1]
threshold = float(sys.argv[2])

print(f'parsing {filename} with threshold {threshold}%')
print()

print('[')

in_js = False

for line in open(sys.argv[1]).readlines():
    if ']:' in line:
        if '[JavaScript]' in line:
            in_js = True
        else:
            in_js = False
    elif in_js:
        if ' ticks ' in line:
            # This is the header.
            continue

        # The JS section looks like this:
        #    N    X%    Y%  Function: *name
        # where N is the # of ticks, X is the total %, Y is the nonlib %, and
        # name is the function name.
        m = re.match('\s*(\d+)\s+([\d.]+)%\s+([\d.]+)%\s+Function: [*]([\w_.$@]+)\s*', line.strip())
        if not m:
            continue
        total = m[2]
        name = m[4]
        if float(total) >= threshold:
            print(f'  "{name}",')

print(']')

