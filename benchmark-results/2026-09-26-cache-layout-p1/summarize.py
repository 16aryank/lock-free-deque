"""Summarize recorded CPU deltas at leaf PCs; no instruction-latency claim."""

import collections
import gzip
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
summary = {}
for path in sorted(ROOT.glob("*.json.gz")):
    with gzip.open(path) as stream:
        profile = json.load(stream)
    symbols = json.loads(path.with_suffix("").with_suffix(".json.syms.json").read_text())
    names = {
        lib["debug_name"]: {
            address: symbols["string_table"][lib["symbol_table"][index]["symbol"]]
            for address, index in lib["known_addresses"]
        }
        for lib in symbols["data"]
    }
    groups = {}
    for group, main in (("owner", True), ("thieves", False)):
        functions = collections.Counter()
        pcs = collections.Counter()
        total = 0
        sample_count = 0
        for thread in profile["threads"]:
            if thread["isMainThread"] != main:
                continue
            for index, stack in enumerate(thread["samples"]["stack"]):
                if stack is None:
                    continue
                delta = thread["samples"]["threadCPUDelta"][index] or 0
                total += delta
                sample_count += 1
                frame = thread["stackTable"]["frame"][stack]
                func = thread["frameTable"]["func"][frame]
                resource = thread["funcTable"]["resource"][func]
                if resource < 0:
                    name = thread["stringArray"][thread["funcTable"]["name"][func]]
                else:
                    lib = profile["libs"][thread["resourceTable"]["lib"][resource]]
                    address = thread["frameTable"]["address"][frame]
                    name = names.get(lib["debugName"], {}).get(address, hex(address))
                    pcs[(lib["debugName"], hex(address))] += delta
                functions[name] += delta
        groups[group] = {
            "cpu_delta_us": total,
            "sample_count": sample_count,
            "leaf_functions": [
                {"name": name, "cpu_delta_us": value, "percent": 100 * value / total}
                for name, value in functions.most_common(12)
            ] if total else [],
            "leaf_pcs": [
                {"library": lib, "rva": pc, "cpu_delta_us": value,
                 "percent": 100 * value / total}
                for (lib, pc), value in pcs.most_common(20)
            ] if total else [],
        }
    summary[path.name] = groups
print(json.dumps(summary, indent=2))
