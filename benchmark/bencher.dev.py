# This script is adapted from the mimalloc-bench project.
# It converts the benchmark outputs to the format required by bencher.dev.

import re
import sys
import collections
try:
    import numpy as np 
except ImportError:
    print('You need to install numpy.')
    sys.exit(1)

if len(sys.argv) != 2:
    print('Usage: %s benchres.csv' % sys.argv[0])
    print('Where benchres.csv is the output of the benchmark script. I.e.')
    print(' mimalloc-bench/out/bench/benchres.csv')
    print()
    print('The script generates a single file for submission to bencher.dev.')
    sys.exit(1)

parse_line = re.compile('^([^ ]+) +([^ ]+) +([0-9:.]+) +([0-9]+)')
data = []
test_names = set()

# read in the data
with open(sys.argv[1]) as f:
    for l in f.readlines():
        match = parse_line.search(l)
        if not match:
            continue
        test_name, alloc_name, time_string, memory = match.groups()
        time_split = time_string.split(':')
        time_taken = 0
        test_names.add(test_name)
        if len(time_split) == 2:
            time_taken = int(time_split[0]) * 60 + float(time_split[1])
        else:
            time_taken = float(time_split[0])
        data.append({"Benchmark":test_name, "Allocator":alloc_name, "Time":time_taken, "Memory":int(memory)})

# Output data in json of the form
#
# {
#    "<Benchmark>":{
#      "memory":{
#        "value": <memory-mean>
#        "high-value": <memory-high>
#        "low-value": <memory-low>
#      }
#      "time":{
#        "value": <time-mean>
#        "high-value": <time-high>
#        "low-value": <time-low>
#      }
#    }
# }

import json

for alloc in set(d["Allocator"] for d in data if d["Benchmark"] == test_name):
    output = {}
    for test_name in test_names:
        output[test_name] = {
            "memory": {
                "value": float(np.mean([d["Memory"] for d in data if d["Benchmark"] == test_name])),
                "high-value": float(np.max([d["Memory"] for d in data if d["Benchmark"] == test_name])),
                "low-value": float(np.min([d["Memory"] for d in data if d["Benchmark"] == test_name])),
            },
            "time": {
                "value": float(np.mean([d["Time"] for d in data if d["Benchmark"] == test_name])),
                "high-value": float(np.max([d["Time"] for d in data if d["Benchmark"] == test_name])),
                "low-value": float(np.min([d["Time"] for d in data if d["Benchmark"] == test_name])),
            }
        }
    result = json.dumps(output, indent=2)
    with open(f"bencher.dev.{alloc}.json", "w") as f:
        f.write(result)
    print(f"Output written to bencher.dev.{alloc}.json")

