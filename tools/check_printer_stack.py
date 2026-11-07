"""Check the deep HTTP Monitor P1S paths in an ESP32/IDF 6.1 build, read-only.

This guards a previously reproduced compiled-stack overflow on the printer HTTP
handlers. It does not replace runtime high-water measurements or bound indirect
calls beyond the listed paths.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

# HTTPD's handler callback down to the configured native runtime callbacks.
HTTPD = (
    "vPortTaskWrapper", "httpd_thread", "httpd_server", "httpd_sess_enum",
    "httpd_process_session", "httpd_sess_process", "httpd_req_new",
    "httpd_parse_req", "httpd_uri",
)
MONITOR_PATHS = {
    "start_status": HTTPD + ("monitor", "mutate", "printer_control_monitor_start",
                             "heat", "app_runtime_get_status", "xQueueSemaphoreTake"),
    "start_permit": HTTPD + ("monitor", "mutate", "printer_control_monitor_start",
                             "begin", "app_runtime_monitor_begin", "xQueueSemaphoreTake"),
    "stop": HTTPD + ("stop", "mutate", "printer_control_monitor_stop",
                     "end_monitor", "app_runtime_monitor_end", "xQueueSemaphoreTake"),
}
# Classic ESP32: 96-byte coprocessor save area + 16-byte TLS in this build,
# plus the 192-byte low-interrupt entry frame before switching to ISR stack.
ABI_AND_INTERRUPT_BYTES = 96 + 16 + 192


def check(build, stack_bytes=4096, margin=512):
    description = json.loads((build / "project_description.json").read_text("utf-8"))
    if description.get("target") != "esp32":
        raise ValueError("stack check requires classic ESP32")
    cache = (build / "CMakeCache.txt").read_text("utf-8")
    match = re.search(r"^CMAKE_OBJDUMP:FILEPATH=(.+)$", cache, re.M)
    if not match:
        raise ValueError("build has no objdump")
    elf = build / (description["project_name"] + ".elf")
    output = subprocess.run([match[1].strip(), "-d", str(elf)],
                            check=True, capture_output=True, text=True).stdout
    frames = {}
    symbol = None
    for line in output.splitlines():
        header = re.match(r"^[0-9a-f]+ <([^>]+)>:$", line)
        if header:
            symbol = header[1]
        entry = re.search(r"\bentry\s+a1,\s*(0x[0-9a-f]+|[0-9]+)", line)
        if entry and symbol:
            frames[symbol] = int(entry[1], 0)
    names = sorted({name for path in MONITOR_PATHS.values() for name in path})
    missing = [name for name in names if name not in frames]
    if missing:
        raise ValueError("reassess compiled path; missing symbols: " + ", ".join(missing))
    paths = {}
    for label, path in MONITOR_PATHS.items():
        call_bytes = sum(frames[name] for name in path)
        paths[label] = {"call_bytes": call_bytes,
                        "required_bytes": call_bytes + ABI_AND_INTERRUPT_BYTES + margin}
    deepest = max(paths, key=lambda label: paths[label]["required_bytes"])
    required = paths[deepest]["required_bytes"]
    result = {"frames": {name: frames[name] for name in names}, "paths": paths,
              "deepest_path": deepest, "abi_interrupt_bytes": ABI_AND_INTERRUPT_BYTES,
              "margin_bytes": margin, "required_bytes": required,
              "http_stack_bytes": stack_bytes, "fits": required <= stack_bytes}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--stack-bytes", type=int, default=4096)
    parser.add_argument("--margin", type=int, default=512)
    args = parser.parse_args()
    result = check(args.build, args.stack_bytes, args.margin)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["fits"] else 1)
