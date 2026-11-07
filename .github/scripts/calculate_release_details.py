import contextlib
import glob
import json
import os
import re
import subprocess
from datetime import UTC, datetime


def run_git(args):
    try:
        return (
            subprocess.check_output(["git"] + args, stderr=subprocess.DEVNULL)
            .decode("utf-8")
            .strip()
        )
    except subprocess.CalledProcessError:
        return ""


# Upstream fork baseline: the original repo and the last upstream tag before this fork
UPSTREAM_REPO = "ErSeraph/switch-assistant"
UPSTREAM_BASELINE_TAG = "v1.8"
UPSTREAM_RELEASE_URL = (
    f"https://github.com/{UPSTREAM_REPO}/releases/tag/{UPSTREAM_BASELINE_TAG}"
)


def main():
    rtype = os.environ.get("RELEASE_TYPE", "beta")
    bump_level = os.environ.get("BUMP_LEVEL", "patch")
    version_override = os.environ.get("VERSION_OVERRIDE", "")
    repo = os.environ.get("REPO", "")

    # Determine owner and repo_name dynamically
    owner = "FaserF"
    repo_name = os.path.basename(os.getcwd())
    if "/" in repo:
        owner, repo_name = repo.split("/", 1)
    if not repo:
        repo = f"{owner}/{repo_name}"

    # Dynamic manifest location
    manifest_files = glob.glob("custom_components/*/manifest.json")
    if manifest_files:
        manifest_path = manifest_files[0]
        domain = os.path.basename(os.path.dirname(manifest_path))
        with open(manifest_path, encoding="utf-8") as f:
            manifest = json.load(f)
        friendly_name = manifest.get("name", domain)
        docs_url = manifest.get("documentation")
    else:
        manifest_path = None
        domain = "switch-assistant"
        friendly_name = "Switch Assistant"
        docs_url = None

    # Dynamic documentation URL logic
    if not docs_url:
        if os.path.exists("docs"):
            docs_url = f"https://{owner}.github.io/{repo_name}/"
        else:
            docs_url = (
                f"https://github.com/{repo}"
                if repo
                else f"https://github.com/FaserF/{repo_name}"
            )

    # Calculate version via version_manager
    bump_args = [
        "python",
        ".github/scripts/version_manager.py",
        "bump",
        "--type",
        rtype,
        "--level",
        bump_level,
    ]
    if version_override and version_override.strip():
        bump_args += ["--override", version_override.strip()]

    try:
        version = subprocess.check_output(bump_args).decode("utf-8").strip()
    except Exception as e:
        print(f"Error calculating version: {e}")
        version = "2.0.0b0"

    # Revert version bump change in modified files (versioning job only calculates; sync-version writes)
    if manifest_path:
        run_git(["checkout", "--", manifest_path])
    if os.path.exists("version.mk"):
        run_git(["checkout", "--", "version.mk"])
    if os.path.exists("pyproject.toml"):
        run_git(["checkout", "--", "pyproject.toml"])

    print(f"Calculated Version: {version}")
    tag = f"v{version}"
    is_prerelease = "false" if rtype == "stable" else "true"

    # Base version without suffixes
    base_version = version
    m = re.match(r"^(\d+\.\d+\.\d+)", version)
    if m:
        base_version = m.group(1)

    changelog_from = ""
    changelog_label = "initial release — full history"
    is_first_release = False

    # Get sorted list of tags
    tags_raw = run_git(["tag", "-l", "[0-9]*", "v[0-9]*", "--sort=-v:refname"])
    tags = [t.strip() for t in tags_raw.splitlines() if t.strip()]
    latest_tag = ""
    for t in tags:
        if re.match(r"^v?\d+\.\d+\.\d+(?:(?:b|-dev|-nightly)\d+)?$", t):
            latest_tag = t
            break

    # Filter out upstream-only tags (1.x without patch, e.g. v1.8 — these were X.Y format, not X.Y.Z)
    # Our own releases are X.Y.Z or X.Y.ZbN; upstream tags are X.Y (two parts only)
    own_tags = [
        t for t in tags if re.match(r"^v?\d+\.\d+\.\d+(?:(?:b|-dev|-nightly)\d+)?$", t)
    ]

    if rtype == "stable":
        for t in own_tags:
            if re.match(r"^v?\d+\.\d+\.\d+$", t):
                changelog_from = t
                changelog_label = f"since last stable release (`{t}`)"
                break
    elif rtype == "beta":
        prev_beta_pat = re.compile(rf"^v?{re.escape(base_version)}(?:b|-beta)\d+$")
        for t in own_tags:
            if prev_beta_pat.match(t):
                changelog_from = t
                changelog_label = f"since previous beta (`{t}`)"
                break
        if not changelog_from:
            for t in own_tags:
                # Skip a stable tag with the same base version as this beta
                # (e.g. v2.0.0 exists as an accidental release, but we're releasing 2.0.0b0)
                t_bare = re.sub(r"^v", "", t)
                if re.match(r"^\d+\.\d+\.\d+$", t_bare) and t_bare != base_version:
                    changelog_from = t
                    changelog_label = f"since last stable release (`{t}`) — first beta of {base_version}"
                    break
    else:
        if latest_tag:
            changelog_from = latest_tag
            changelog_label = f"since `{latest_tag}`"

    # Detect first-ever own release (no own tags, or beta whose only "stable" was the same
    # base version = accidental release): fall back to upstream fork baseline.
    if not changelog_from:
        is_first_release = True
        changelog_from = UPSTREAM_BASELINE_TAG
        changelog_label = (
            f"initial release — based on upstream fork "
            f"[{UPSTREAM_BASELINE_TAG}]({UPSTREAM_RELEASE_URL})"
        )

    print(f"Changelog range start tag: '{changelog_from}' ({changelog_label})")

    # Count commits
    total_commit_count = 0
    count_range = f"{changelog_from}..HEAD" if changelog_from else "HEAD"

    commit_count_raw = run_git(["rev-list", "--count", count_range])
    if commit_count_raw.isdigit():
        total_commit_count = int(commit_count_raw)

    # Generate Changelog using changelog_builder.py (preferred) or fallback
    changelog_md = ""
    script_path = None
    if os.path.exists(".github/scripts/changelog_builder.py"):
        script_path = ".github/scripts/changelog_builder.py"
    elif os.path.exists("scripts/generate_changelog.py"):
        script_path = "scripts/generate_changelog.py"

    if script_path:
        try:
            repo_url = (
                f"https://github.com/{repo}"
                if repo
                else f"https://github.com/{owner}/{repo_name}"
            )
            cl_args = [
                "python",
                script_path,
                "--repo-url",
                repo_url,
                "--output",
                "CHANGELOG_BODY.md",
            ]
            if changelog_from:
                cl_args.extend(["--from-tag", changelog_from])
            # Pass upstream baseline info for first-release note
            if (
                is_first_release
                and "--upstream-ref" in open(script_path, encoding="utf-8").read()
            ):
                cl_args.extend(["--upstream-ref", UPSTREAM_BASELINE_TAG])
                cl_args.extend(["--upstream-url", UPSTREAM_RELEASE_URL])
            subprocess.run(cl_args, check=True)
            if os.path.exists("CHANGELOG_BODY.md"):
                with open("CHANGELOG_BODY.md", encoding="utf-8") as cl_file:
                    changelog_md = cl_file.read().strip()
                with contextlib.suppress(OSError):
                    os.remove("CHANGELOG_BODY.md")
        except Exception as e:  # noqa: BLE001
            print(f"Error generating changelog: {e}")
            changelog_md = (
                "_Changelog could not be generated automatically. See commit history._"
            )
    else:
        changelog_md = "_Changelog script not found._"

    if not changelog_md:
        changelog_md = "_No categorised changes detected._"

    # Channel decorations
    channel_badge = {
        "stable": "![Stable](https://img.shields.io/badge/channel-stable-brightgreen?style=flat-square)",
        "beta": "![Beta](https://img.shields.io/badge/channel-beta-orange?style=flat-square)",
    }.get(
        rtype,
        "![Nightly](https://img.shields.io/badge/channel-nightly-blue?style=flat-square)",
    )

    # Analyze diff impact
    diff_range = f"{changelog_from}..HEAD" if changelog_from else "HEAD"
    changed_files_raw = run_git(["diff", "--name-only", diff_range])
    changed_files = [f.strip() for f in changed_files_raw.splitlines() if f.strip()]

    total_files = len(changed_files)
    integration_count = 0
    translation_count = 0
    ci_count = 0
    docs_count = 0
    test_count = 0

    for f in changed_files:
        if f.startswith(f"custom_components/{domain}/translations/"):
            translation_count += 1
        elif f.startswith("custom_components/") or f.startswith(
            ("source/", "include/", "sysmodule/", "overlay/", "overlay-loader/")
        ):
            integration_count += 1
        elif f.startswith("tests/"):
            test_count += 1
        elif f.startswith((".github/", "scripts/")):
            ci_count += 1
        elif f.startswith("docs/") or f.endswith(".md"):
            docs_count += 1

    breaking_count = 0
    log_msgs = run_git(["log", diff_range, "--format=%B"])
    for msg in log_msgs.split("\n"):
        if re.search(r"\bBREAKING CHANGE\b|\bBREAKING:\b|^[a-zA-Z]+!:", msg):
            breaking_count += 1

    # Determine Risk Severity
    severity = "Low"
    alert_type = "NOTE"
    preamble = "This release introduces minor updates and code improvements."

    if breaking_count > 0:
        severity = "Critical"
        alert_type = "CAUTION"
        preamble = f"This release contains **{breaking_count} breaking change(s)**! Please review the changelog carefully before updating."
    elif integration_count > 8:
        severity = "High"
        alert_type = "WARNING"
        preamble = "This release contains significant changes to core features. Please verify integration behavior after updating."
    elif integration_count > 2 or translation_count > 5:
        severity = "Medium"
        alert_type = "TIP"
        preamble = "This release contains standard updates and feature enhancements to the integration logic or translations."

    if rtype != "stable":
        preamble = f"ℹ️ **This is a {rtype} build.** It contains preview features for testing.<br><br>{preamble}"

    impact_summary = []
    if total_files > 0:
        if integration_count > 0:
            pct = round((integration_count / total_files) * 100)
            impact_summary.append(f"⚙️ Core ({integration_count} files · {pct}%)")
        if translation_count > 0:
            pct = round((translation_count / total_files) * 100)
            impact_summary.append(
                f"🗣️ Translations ({translation_count} files · {pct}%)"
            )
        if test_count > 0:
            pct = round((test_count / total_files) * 100)
            impact_summary.append(f"🧪 Tests ({test_count} files · {pct}%)")
        if ci_count > 0:
            pct = round((ci_count / total_files) * 100)
            impact_summary.append(f"🚀 CI/CD ({ci_count} files · {pct}%)")
        if docs_count > 0:
            pct = round((docs_count / total_files) * 100)
            impact_summary.append(f"📖 Docs ({docs_count} files · {pct}%)")

    impact_str = (
        " · ".join(impact_summary)
        if impact_summary
        else "No codebase changes detected."
    )

    prerelease_note = (
        f"\n> [!{alert_type}]\n"
        f"> **Release Risk: {severity}**\n"
        f"> {preamble}\n"
        f">\n"
        f"> **Affected areas:** {impact_str}\n"
    )

    # Download asset filename
    download_asset = f"{domain}.zip" if manifest_path else "switch-ha.nro"
    released_at = datetime.now(UTC).strftime("%Y-%m-%d %H:%M") + " UTC"

    body_parts = [
        f"# {friendly_name} {version}  {channel_badge}",
        "",
        prerelease_note,
        "## 📋 What's Changed",
        "",
        changelog_md,
        "",
        "## 📊 Release Details",
        "",
        "| | |",
        "|---|---|",
        f"| **Version** | `{version}` |",
        f"| **Channel** | {rtype} |",
        f"| **Released** | {released_at} |",
        f"| **Commits included** | {total_commit_count} — {changelog_label} |",
        f"| **Downloads (this release)** | [![Downloads](https://img.shields.io/github/downloads/{owner}/{repo_name}/{tag}/{download_asset}?style=flat-square&logo=github)](https://github.com/{owner}/{repo_name}/releases/tag/{tag}) |",
        f"| **Downloads (total)** | [![Downloads](https://img.shields.io/github/downloads/{owner}/{repo_name}/total?style=flat-square&logo=github)](https://github.com/{owner}/{repo_name}/releases) |",
        "",
        "---",
        "",
        f"*📖 [Documentation]({docs_url})  ·  🐛 [Report an Issue](https://github.com/{repo}/issues/new/choose)  ·  📦 [All Releases](https://github.com/{repo}/releases)*",
    ]

    body = "\n".join(body_parts)
    with open("release_body.md", "w", encoding="utf-8") as f:
        f.write(body)

    # Output parameters for GITHUB_OUTPUT
    github_output = os.environ.get("GITHUB_OUTPUT", "")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as f:
            f.write(f"version={version}\n")
            f.write(f"tag={tag}\n")
            f.write(f"is_prerelease={is_prerelease}\n")
            import uuid

            delimiter = f"gh_release_{uuid.uuid4().hex}"
            f.write(f"release_body<<{delimiter}\n")
            f.write(body + "\n")
            f.write(f"{delimiter}\n")


if __name__ == "__main__":
    main()
