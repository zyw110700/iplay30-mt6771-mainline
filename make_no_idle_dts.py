#!/usr/bin/env python3
"""Regenerate vendor_no_idle.dts from vendor_full.dts.

The vendor DTB's cpu-idle-states point at MTK SPM PSCI deep-sleep states that
the mainline kernel cannot wake from (see HANDOVER 6.20).  Strip the property
from every CPU and drop the whole /cpus/idle-states node, so the kernel falls
back to plain WFI.
"""
import sys

src = open('vendor_full.dts').read()

prop = '\t\t\tcpu-idle-states = <0x02 0x03 0x04 0x05 0x06 0x07 0x08>;\n'
n = src.count(prop)
src = src.replace(prop, '')

start = src.index('\t\tidle-states {')
i = src.index('{', start)
depth = 0
j = i
while True:
    if src[j] == '{':
        depth += 1
    elif src[j] == '}':
        depth -= 1
        if depth == 0:
            break
    j += 1
end = src.index(';', j) + 1
if src[end] == '\n':
    end += 1
src = src[:start] + src[end:]

# Drop the now dangling __symbols__ entries.
lines = src.splitlines(keepends=True)
lines = [l for l in lines if '/cpus/idle-states/' not in l]
src = ''.join(lines)

open('vendor_no_idle.dts', 'w').write(src)
print(f'wrote vendor_no_idle.dts (removed {n} cpu-idle-states properties)')
