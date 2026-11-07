import argparse
import re
import subprocess


def get_commits(tag=None):
    cmd = ["git", "log", f"{tag}..HEAD" if tag else "HEAD", "--pretty=format:%H|%h|%s"]
    try:
        out = subprocess.check_output(cmd, text=True).strip()
        return out.split("\n") if out else []
    except subprocess.SubprocessError, OSError:
        return []


def build_changelog(commits, url, upstream_ref=None, upstream_url=None):
    cats = {
        "✨ Features": [],
        "🐛 Bug Fixes": [],
        "📦 Dependencies": [],
        "🔧 Maintenance & CI": [],
        "🚀 Other": [],
    }
    for line in commits:
        if "|" not in line:
            continue
        fh, sh, sub = line.split("|", 2)
        sl = sub.lower()
        if any(
            x in sl for x in ["chore: release", "chore: bump", "merge ", "[skip ci]"]
        ):
            continue
        sub = re.sub(r"\(#(\d+)\)", rf"([#\1]({url}/pull/\1))", sub)
        entry = f"- {sub} ([{sh}]({url}/commit/{fh}))"
        if (
            re.match(r"^(feat|add|added|adding|new|feature|i18n|translation|✨)", sl)
            or "translation" in sl
        ):
            cats["✨ Features"].append(entry)
        elif re.match(
            r"^(fix|fixing|fixed|fixes|bug|patch|repair|resolve|resolved|🐛)", sl
        ):
            cats["🐛 Bug Fixes"].append(entry)
        elif re.match(r"^(deps|dep|update|bump|renovate|📦|⬆️)", sl):
            cats["📦 Dependencies"].append(entry)
        elif re.match(r"^(chore|ci|workflow|config|ruff|🔧)", sl):
            cats["🔧 Maintenance & CI"].append(entry)
        else:
            cats["🚀 Other"].append(entry)

    changelog = "## Changelog\n\n"
    for title, items in cats.items():
        if items:
            changelog += f"### {title}\n" + "\n".join(items) + "\n\n"

    # If this is the first release of a fork, note the upstream baseline
    if upstream_ref and upstream_url:
        changelog += (
            f"\n---\n\n"
            f"> **ℹ️ First release note:** This is the first release of this fork. "
            f"It is based on upstream [{upstream_ref}]({upstream_url}) as the baseline. "
            f"All changes above are relative to that release.\n"
        )

    return changelog or "No significant changes."


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--from-tag")
    p.add_argument("--repo-url", required=True)
    p.add_argument("--output", default="CHANGELOG_BODY.md")
    p.add_argument(
        "--upstream-ref",
        default=None,
        help="Upstream fork baseline tag/version (e.g. v1.8)",
    )
    p.add_argument(
        "--upstream-url",
        default=None,
        help="Upstream repo URL for linking to baseline release",
    )
    args = p.parse_args()
    commits = get_commits(args.from_tag)
    changelog = build_changelog(
        commits, args.repo_url, args.upstream_ref, args.upstream_url
    )
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(changelog)
