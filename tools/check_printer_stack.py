"""Check the known deep HTTP arm path in an ESP32/IDF 6.1 build, read-only.

This guards a reproduced compiled-stack overflow. It does not replace runtime
high-water measurements or bound indirect calls beyond the listed path.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

# Includes HTTPD's handler callback and the configured native heat callback.
ARM_PATH = (
    "vPortTaskWrapper", "httpd_thread", "httpd_server", "httpd_sess_enum",
    "httpd_process_session", "httpd_sess_process", "httpd_req_new",
    "httpd_parse_req", "httpd_uri", "arm", "mutate", "printer_control_arm",
    "input", "heat", "app_runtime_get_status", "xQueueSemaphoreTake",
)
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
    missing = [name for name in ARM_PATH if name not in frames]
    if missing:
        raise ValueError("reassess compiled path; missing symbols: " + ", ".join(missing))
    call_bytes = sum(frames[name] for name in ARM_PATH)
    required = call_bytes + ABI_AND_INTERRUPT_BYTES + margin
    result = {"frames": {name: frames[name] for name in ARM_PATH},
              "call_bytes": call_bytes, "abi_interrupt_bytes": ABI_AND_INTERRUPT_BYTES,
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
