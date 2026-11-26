"""Check committed release metadata before a GitHub Actions release."""
import argparse
import os
from pathlib import Path
import subprocess

from package_firmware import RELEASE_REPOSITORY, require, stable_version


def git(root, *arguments):
    result = subprocess.run(["git", "-C", str(root), *arguments],
                            capture_output=True, text=True)
    require(result.returncode == 0, "Could not read Git source state")
    return result.stdout.strip()


def release_plan(root, ref, repository, commit, stage_only=False):
    require(repository == RELEASE_REPOSITORY, "Releases require the upstream repository")
    require(ref == "refs/heads/main", "Releases must run from main")
    require(commit and git(root, "rev-parse", "HEAD") == commit,
            "Checkout does not match the workflow source commit")
    require(not git(root, "status", "--porcelain=v1", "--untracked-files=normal"),
            "Release source must be clean and committed")
    version = (root / "version.txt").read_text(encoding="utf-8").strip()
    stable_version(version)
    tag = f"v{version}"
    notes = root / "docs/releases" / f"{tag}.md"
    if stage_only:
        require(notes.is_file() and notes.read_text(encoding="utf-8").strip(),
                f"Add nonempty release notes at docs/releases/{tag}.md")
        return {"version": version, "tag": tag, "commit": commit, "publish": "false", "build": "true"}
    tags = git(root, "tag", "--list").splitlines()
    plan = {"version": version, "tag": tag, "commit": commit, "publish": "false", "build": "false"}
    if tag in tags:
        return plan

    previous = []
    for existing in tags:
        candidate = existing.removeprefix("v")
        try:
            stable_version(candidate)
        except ValueError:
            continue
        previous.append(tuple(int(part) for part in candidate.split(".")))
    require(not previous or tuple(int(part) for part in version.split(".")) > max(previous),
            "Release version must be newer than every existing stable version tag")
    require(notes.is_file() and notes.read_text(encoding="utf-8").strip(),
            f"Add nonempty release notes at docs/releases/{tag}.md")
    plan["publish"] = "true"
    plan["build"] = "true"
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--ref", default=os.environ.get("GITHUB_REF"))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--commit", default=os.environ.get("GITHUB_SHA"))
    parser.add_argument("--github-output", type=Path, default=os.environ.get("GITHUB_OUTPUT"))
    parser.add_argument("--stage-only", action="store_true", help="sign a package without publishing")
    arguments = parser.parse_args()
    try:
        plan = release_plan(arguments.root, arguments.ref, arguments.repository, arguments.commit, arguments.stage_only)
        if arguments.github_output:
            with arguments.github_output.open("a", encoding="utf-8", newline="\n") as output:
                output.write("".join(f"{name}={value}\n" for name, value in plan.items()))
        if arguments.stage_only:
            print(f"Preparing signed assets for {plan['tag']} from {plan['commit']}")
        elif plan["publish"] == "true":
            print(f"Ready to release {plan['tag']} from {plan['commit']}")
        else:
            print(f"Skipping {plan['tag']}: tag already exists; nothing will be overwritten")
    except (ValueError, OSError) as error:
        parser.exit(1, f"Release rejected: {error}\n")


if __name__ == "__main__":
    main()
