import json
import os
import re
import subprocess
import sys


def get_latest_tag():
    try:
        # Get all tags sorted by version-like order
        result = subprocess.run(
            ["git", "tag", "--list", "--sort=-v:refname"],
            capture_output=True,
            text=True,
            check=True,
        )
        tags = result.stdout.strip().split("\n")
        return tags[0] if tags and tags[0] else None
    except Exception:  # noqa: BLE001
        return None


def parse_version(v_str):
    # Parses X.Y.Z-beta.N or X.Y.Z
    # Returns (major, minor, patch, is_beta, beta_num)
    if not v_str:
        return (1, 0, 0, False, -1)

    core = v_str.split("-")[0]
    parts = list(map(int, core.split(".")))
    while len(parts) < 3:
        parts.append(0)

    is_beta = "-" in v_str and "beta" in v_str
    beta_num = -1
    if is_beta:
        match = re.search(r"beta\.(\d+)", v_str)
        if match:
            beta_num = int(match.group(1))

    return parts[0], parts[1], parts[2], is_beta, beta_num


def bump_version(current, bump_type, release_status):
    is_target_beta = release_status == "beta"

    if not current:
        if is_target_beta:
            return "1.0.0-beta.0"
        return "1.0.0"

    major, minor, patch, is_curr_beta, curr_beta_num = parse_version(current)

    # Calculate Target Stable Core
    if bump_type == "major":
        target_core = (major + 1, 0, 0)
    elif bump_type == "minor":
        target_core = (major, minor + 1, 0)
    else:  # patch
        target_core = (major, minor, patch + 1)

    target_core_str = f"{target_core[0]}.{target_core[1]}.{target_core[2]}"

    if is_target_beta:
        # If current version's core matches target_core and is a beta, increment beta_num
        current_core = (major, minor, patch)
        if current_core == target_core and is_curr_beta:
            return f"{target_core_str}-beta.{curr_beta_num + 1}"
        else:
            return f"{target_core_str}-beta.0"
    else:
        return target_core_str


def update_files(new_version):
    # Update pyproject.toml
    if os.path.exists("pyproject.toml"):
        with open("pyproject.toml") as f:
            content = f.read()
        content = re.sub(
            r'version\s*=\s*"[^"]+"', f'version = "{new_version}"', content, count=1
        )
        with open("pyproject.toml", "w") as f:
            f.write(content)

    # Update manifest.json
    manifest_path = "custom_components/switch_cfw/manifest.json"
    if os.path.exists(manifest_path):
        with open(manifest_path) as f:
            manifest = json.load(f)
        manifest["version"] = new_version
        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")

    # Update const.py
    const_path = "custom_components/switch_cfw/const.py"
    if os.path.exists(const_path):
        with open(const_path) as f:
            content = f.read()
        # Update all typical version locations in const.py
        content = re.sub(
            r"Version: \d+\.\d+\.\d+(-[a-z0-9.]+)?", f"Version: {new_version}", content
        )
        content = re.sub(
            r'ATTR_FIRMWARE_VERSION = "[^"]+"',
            f'ATTR_FIRMWARE_VERSION = "{new_version}"',
            content,
        )
        content = re.sub(
            r'ATTR_APP_VERSION = "[^"]+"',
            f'ATTR_APP_VERSION = "{new_version}"',
            content,
        )
        content = re.sub(
            r'MIN_APP_VERSION = "[^"]+"', f'MIN_APP_VERSION = "{new_version}"', content
        )
        with open(const_path, "w") as f:
            f.write(content)

    # Update switch_sysmodule/homeassistant_sysmodule.json
    sys_json_path = "switch_sysmodule/homeassistant_sysmodule.json"
    if os.path.exists(sys_json_path):
        with open(sys_json_path) as f:
            sys_json = json.load(f)
        sys_json["version"] = new_version
        with open(sys_json_path, "w") as f:
            json.dump(sys_json, f, indent=4)
            f.write("\n")

    # Update switch_sysmodule Makefile
    sys_makefile = "switch_sysmodule/Makefile"
    if os.path.exists(sys_makefile):
        with open(sys_makefile) as f:
            content = f.read()
        content = re.sub(
            r"APP_VERSION\s*?[:?]?=\s*?.*",
            f"APP_VERSION\t:=\t{new_version}",
            content,
            count=1,
        )
        with open(sys_makefile, "w") as f:
            f.write(content)

    # Update switch_app Makefile
    app_makefile_path = "switch_app/Makefile"
    if os.path.exists(app_makefile_path):
        with open(app_makefile_path) as f:
            content = f.read()
        content = re.sub(
            r"VERSION\s*?[:?]?=\s*?.*", f"VERSION\t?=\t{new_version}", content, count=1
        )
        with open(app_makefile_path, "w") as f:
            f.write(content)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python bump_version.py <major|minor|patch> <stable|beta>")
        sys.exit(1)

    bump_type = sys.argv[1].lower()
    release_status = sys.argv[2].lower()

    latest_tag = get_latest_tag()
    current_v = latest_tag.lstrip("v") if latest_tag else None

    new_v = bump_version(current_v, bump_type, release_status)
    print(f"New Version: {new_v}")

    update_files(new_v)

    # Write to file for GitHub Actions
    with open("VERSION.txt", "w") as f:
        f.write(new_v)
