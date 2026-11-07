import json
import os
import re
import sys


def update_files(new_version):
    print(f"Updating all project files to version {new_version}...")

    # 1. pyproject.toml
    if os.path.exists("pyproject.toml"):
        with open("pyproject.toml", encoding="utf-8") as f:
            content = f.read()
        content = re.sub(
            r'(version\s*=\s*")[^"]+(")', rf"\g<1>{new_version}\g<2>", content, count=1
        )
        with open("pyproject.toml", "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print("- Updated pyproject.toml")

    # 2. manifest.json
    manifest_path = "custom_components/switch_cfw/manifest.json"
    if os.path.exists(manifest_path):
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)
        manifest["version"] = new_version
        with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest, f, indent=2)
            f.write("\n")
        print("- Updated manifest.json")

    # 3. const.py
    const_path = "custom_components/switch_cfw/const.py"
    if os.path.exists(const_path):
        with open(const_path, encoding="utf-8") as f:
            content = f.read()
        content = re.sub(
            r"(Version: )\d+\.\d+\.\d+(-[a-z0-9.]+)?", rf"\g<1>{new_version}", content
        )
        content = re.sub(
            r'(ATTR_FIRMWARE_VERSION\s*=\s*")[^"]+(")',
            rf"\g<1>{new_version}\g<2>",
            content,
        )
        content = re.sub(
            r'(ATTR_APP_VERSION\s*=\s*")[^"]+(")', rf"\g<1>{new_version}\g<2>", content
        )
        content = re.sub(
            r'(MIN_APP_VERSION\s*=\s*")[^"]+(")', rf"\g<1>{new_version}\g<2>", content
        )
        content = re.sub(
            r'(VERSION\s*=\s*")[^"]+(")', rf"\g<1>{new_version}\g<2>", content
        )
        with open(const_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print("- Updated const.py")

    # 4. switch_sysmodule/main.cpp
    sys_main = "switch_sysmodule/main.cpp"
    if os.path.exists(sys_main):
        with open(sys_main, encoding="utf-8") as f:
            content = f.read()
        content = re.sub(
            r'(#define\s+APP_VERSION\s+")[^"]+(")', rf"\g<1>{new_version}\g<2>", content
        )
        with open(sys_main, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print("- Updated switch_sysmodule/main.cpp")

    # 5. switch_app/main.cpp
    app_main = "switch_app/main.cpp"
    if os.path.exists(app_main):
        with open(app_main, encoding="utf-8") as f:
            content = f.read()
        content = re.sub(
            r'(#define\s+APP_VERSION\s+")[^"]+(")', rf"\g<1>{new_version}\g<2>", content
        )
        with open(app_main, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print("- Updated switch_app/main.cpp")

    # 6. build_switch.ps1
    ps1_path = "build_switch.ps1"
    if os.path.exists(ps1_path):
        with open(ps1_path, encoding="utf-8") as f:
            content = f.read()
        content = re.sub(
            r'(\[string\]\$Version\s*=\s*")[^"]+(")',
            rf"\g<1>{new_version}\g<2>",
            content,
        )
        with open(ps1_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print("- Updated build_switch.ps1")

    # 7. version.mk
    if os.path.exists("version.mk"):
        with open("version.mk", encoding="utf-8") as f:
            content = f.read()
        content = re.sub(
            r"(APP_VERSION\s*\??=\s*)[^\r\n]+",
            rf"\g<1>{new_version}",
            content,
        )
        with open("version.mk", "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print("- Updated version.mk")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python scripts/set_version.py <version>")
        sys.exit(1)
    update_files(sys.argv[1])
