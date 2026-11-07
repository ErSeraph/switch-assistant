import json
import os
import sys


def validate_main_json(filepath):
    if not os.path.exists(filepath):
        print(f"ERROR: {filepath} not found!")
        sys.exit(1)

    with open(filepath) as f:
        try:
            data = json.load(f)
        except json.JSONDecodeError as e:
            print(f"ERROR: Invalid JSON in {filepath}. Details: {e}")
            sys.exit(1)

    print(f"Validating {filepath}...")

    # Required modern NPDM JSON fields
    required_fields = [
        "name",
        "program_id",
        "program_id_range_min",
        "program_id_range_max",
        "version",
        "system_resource_size",
        "process_category",
        "optimize_memory_allocation",
        "disable_device_address_space_merge",
        "enable_alias_region_extra_size",
        "prevent_code_reads",
        "signature_key_generation",
        "main_thread_stack_size",
        "main_thread_priority",
        "default_cpu_id",
        "is_retail",
        "pool_partition",
        "is_64_bit",
        "address_space_type",
        "filesystem_access",
        "service_access",
        "kernel_capabilities",
    ]

    missing_fields = [f for f in required_fields if f not in data]
    if missing_fields:
        print(f"ERROR: {filepath} is missing required NPDM fields: {missing_fields}")
        sys.exit(1)

    # Validate kernel capabilities
    kernel_caps = data.get("kernel_capabilities", [])
    syscalls_dict = {}
    for cap in kernel_caps:
        if cap.get("type") == "syscalls":
            syscalls_dict = cap.get("value", {})
            break

    if not syscalls_dict:
        print("ERROR: No syscalls found in kernel_capabilities!")
        sys.exit(1)

    # Check for crucial syscalls needed by sysmodule
    crucial_syscalls = [
        "svcCreateThread",
        "svcSleepThread",
        "svcGetInfo",
        "svcCreateEvent",
        "svcGetProcessList",
    ]

    missing = []
    for syscall in crucial_syscalls:
        if syscall not in syscalls_dict:
            missing.append(syscall)

    if missing:
        print(f"ERROR: Sysmodule is missing critical syscalls: {missing}")
        sys.exit(1)

    # Check for correct optimization patches
    if data.get("disable_device_address_space_merge") is not True:
        print(
            "ERROR: 'disable_device_address_space_merge' must be true for modern sysmodules/loaders."
        )
        sys.exit(1)

    print(f"SUCCESS: {filepath} passed validation.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python validate_sysmodule.py <path_to_main.json>")
        sys.exit(1)

    validate_main_json(sys.argv[1])
