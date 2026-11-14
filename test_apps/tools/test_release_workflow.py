"""Release eligibility checks use real Git state, without contacting GitHub."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools/release_workflow.py"


class ReleaseWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="p1s-release-policy-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        self.output = Path(self.temporary.name) / "github-output"
        self.git("init", "-q")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release-test@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "tag.gpgsign", "false")
        self.write_source("1.0.1")
        self.git("tag", "v1.0.0")

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.root), *arguments],
                              check=True, capture_output=True, text=True).stdout.strip()

    def write_source(self, version, notes="Release test notes.\n"):
        (self.root / "version.txt").write_text(version + "\n", encoding="utf-8")
        if notes is not None:
            path = self.root / "docs/releases" / f"v{version}.md"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(notes, encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-qm", "Prepare release")
        self.commit = self.git("rev-parse", "HEAD")

    def run_policy(self, **overrides):
        arguments = {"root": str(self.root), "ref": "refs/heads/main",
                     "repository": "ahmaddawod12/P1S-HeatControl",
                     "commit": self.commit, "github-output": str(self.output)}
        arguments.update(overrides)
        command = [sys.executable, str(SCRIPT)]
        for key, value in arguments.items():
            command.extend([f"--{key}", value])
        return subprocess.run(command, capture_output=True, text=True)

    def values(self):
        return dict(line.split("=", 1) for line in
                    self.output.read_text(encoding="utf-8").splitlines())

    def assert_rejected(self, expected, **overrides):
        result = self.run_policy(**overrides)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(expected, result.stderr)
        self.assertFalse(self.output.exists(), "Rejected releases must not emit workflow outputs")

    def test_new_version_outputs_exact_source_and_tag(self):
        result = self.run_policy()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.values(), {"version": "1.0.1", "tag": "v1.0.1",
                                        "commit": self.commit, "publish": "true"})

    def test_existing_tag_is_a_successful_noop(self):
        for annotated in (False, True):
            with self.subTest(annotated=annotated):
                if annotated:
                    self.git("tag", "-a", "v1.0.1", "-m", "Existing release")
                else:
                    self.git("tag", "v1.0.1")
                try:
                    result = self.run_policy()
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(self.values()["publish"], "false")
                    self.assertIn("already exists", result.stdout)
                finally:
                    self.git("tag", "-d", "v1.0.1")
                    self.output.unlink(missing_ok=True)

    def test_versions_are_compared_numerically(self):
        self.git("tag", "v1.0.9")
        self.git("tag", "v99.0.0-rc1")
        self.write_source("1.0.10")
        result = self.run_policy()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.values()["version"], "1.0.10")

    def test_downgrade_is_rejected(self):
        self.git("tag", "v1.1.0")
        self.assert_rejected("newer than")

    def test_unprefixed_stable_tag_also_prevents_downgrade(self):
        self.git("tag", "2.0.0")
        self.assert_rejected("newer than")

    def test_malformed_versions_are_rejected(self):
        for version in ("01.0.1", "1.0.2-rc1", "1.0", "4294967296.0.1"):
            with self.subTest(version=version):
                self.write_source(version)
                self.assert_rejected("version")

    def test_release_notes_must_exist_and_contain_text(self):
        self.write_source("1.0.2", notes=None)
        self.assert_rejected("release notes")
        self.write_source("1.0.3", notes=" \n")
        self.assert_rejected("release notes")

    def test_tracked_source_changes_are_rejected(self):
        (self.root / "version.txt").write_text("1.0.2\n", encoding="utf-8")
        self.assert_rejected("clean")

    def test_untracked_source_changes_are_rejected(self):
        (self.root / "uncommitted.c").write_text("int changed;\n", encoding="utf-8")
        self.assert_rejected("clean")

    def test_non_main_ref_is_rejected(self):
        self.assert_rejected("main", ref="refs/heads/feature")

    def test_other_repositories_are_rejected(self):
        self.assert_rejected("repository", repository="fork-owner/P1S-HeatControl")

    def test_wrong_source_commit_is_rejected(self):
        self.assert_rejected("commit", commit="a" * 40)


if __name__ == "__main__":
    unittest.main()
