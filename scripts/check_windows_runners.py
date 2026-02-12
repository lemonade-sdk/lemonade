#!/usr/bin/env python3
"""
Check what Windows self-hosted runners are working on.

This script queries the GitHub API to get information about self-hosted runners
and their current job assignments, filtering for Windows runners.

Usage:
    python check_windows_runners.py --token YOUR_GITHUB_TOKEN
    python check_windows_runners.py --token YOUR_GITHUB_TOKEN --repo owner/repo
    python check_windows_runners.py --token YOUR_GITHUB_TOKEN --org organization
"""

import argparse
import json
import sys
from datetime import datetime
from typing import Dict, List, Optional
import urllib.request
import urllib.error


class GitHubRunnerChecker:
    """Check GitHub self-hosted runners and their current jobs."""

    def __init__(self, token: str, repo: Optional[str] = None, org: Optional[str] = None):
        self.token = token
        self.repo = repo
        self.org = org
        self.base_url = "https://api.github.com"
        self.headers = {
            "Authorization": f"token {token}",
            "Accept": "application/vnd.github.v3+json",
        }

    def _make_request(self, url: str) -> Dict:
        """Make a request to the GitHub API."""
        try:
            req = urllib.request.Request(url, headers=self.headers)
            with urllib.request.urlopen(req) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as e:
            error_body = e.read().decode()
            print(f"Error: HTTP {e.code} - {error_body}", file=sys.stderr)
            raise
        except Exception as e:
            print(f"Error making request to {url}: {e}", file=sys.stderr)
            raise

    def get_runners(self) -> List[Dict]:
        """Get list of self-hosted runners."""
        runners = []

        if self.repo:
            url = f"{self.base_url}/repos/{self.repo}/actions/runners"
            print(f"Fetching runners for repository: {self.repo}")
            data = self._make_request(url)
            runners.extend(data.get("runners", []))

        if self.org:
            url = f"{self.base_url}/orgs/{self.org}/actions/runners"
            print(f"Fetching runners for organization: {self.org}")
            data = self._make_request(url)
            runners.extend(data.get("runners", []))

        # Filter for Windows runners
        windows_runners = [
            r for r in runners
            if any(label.get("name", "").lower() in ["windows", "windows-latest"]
                   or "windows" in label.get("name", "").lower()
                   for label in r.get("labels", []))
        ]

        return windows_runners

    def get_workflow_runs(self) -> List[Dict]:
        """Get recent workflow runs."""
        if not self.repo:
            print("Warning: Repository not specified, cannot fetch workflow runs", file=sys.stderr)
            return []

        url = f"{self.base_url}/repos/{self.repo}/actions/runs?status=in_progress&per_page=100"
        print(f"Fetching in-progress workflow runs...")
        data = self._make_request(url)
        return data.get("workflow_runs", [])

    def get_jobs_for_run(self, run_id: int) -> List[Dict]:
        """Get jobs for a specific workflow run."""
        if not self.repo:
            return []

        url = f"{self.base_url}/repos/{self.repo}/actions/runs/{run_id}/jobs"
        data = self._make_request(url)
        return data.get("jobs", [])

    def format_runner_info(self, runner: Dict) -> str:
        """Format runner information for display."""
        name = runner.get("name", "Unknown")
        status = runner.get("status", "Unknown")
        busy = runner.get("busy", False)
        os = runner.get("os", "Unknown")

        labels = ", ".join([label.get("name", "") for label in runner.get("labels", [])])

        info = [
            f"Runner: {name}",
            f"  Status: {status}",
            f"  Busy: {busy}",
            f"  OS: {os}",
            f"  Labels: {labels}",
        ]

        return "\n".join(info)

    def find_runner_jobs(self, runner_name: str, workflow_runs: List[Dict]) -> List[Dict]:
        """Find jobs assigned to a specific runner."""
        jobs = []
        for run in workflow_runs:
            run_jobs = self.get_jobs_for_run(run["id"])
            for job in run_jobs:
                # Check if job is running on this runner
                runner_name_from_job = job.get("runner_name", "")
                if runner_name == runner_name_from_job and job.get("status") == "in_progress":
                    jobs.append({
                        "job": job,
                        "run": run,
                    })
        return jobs

    def format_job_info(self, job_data: Dict) -> str:
        """Format job information for display."""
        job = job_data["job"]
        run = job_data["run"]

        job_name = job.get("name", "Unknown")
        workflow_name = run.get("name", "Unknown")
        started_at = job.get("started_at", "Unknown")
        run_url = job.get("html_url", "")

        info = [
            f"  Job: {job_name}",
            f"  Workflow: {workflow_name}",
            f"  Started: {started_at}",
            f"  URL: {run_url}",
        ]

        return "\n".join(info)

    def check_windows_runners(self):
        """Main function to check Windows runners and their jobs."""
        print("=" * 80)
        print("Windows Self-Hosted Runners Status")
        print("=" * 80)
        print()

        runners = self.get_runners()

        if not runners:
            print("No Windows self-hosted runners found.")
            return

        print(f"Found {len(runners)} Windows self-hosted runner(s)\n")

        workflow_runs = self.get_workflow_runs() if self.repo else []

        for runner in runners:
            print(self.format_runner_info(runner))

            if runner.get("busy", False):
                runner_name = runner.get("name", "")
                jobs = self.find_runner_jobs(runner_name, workflow_runs)

                if jobs:
                    print("  Currently working on:")
                    for job_data in jobs:
                        print(self.format_job_info(job_data))
                else:
                    print("  Status: Busy (but job details not found in recent runs)")
            else:
                print("  Status: Idle")

            print()


def main():
    parser = argparse.ArgumentParser(
        description="Check what Windows self-hosted GitHub runners are working on"
    )
    parser.add_argument(
        "--token",
        required=True,
        help="GitHub personal access token with 'repo' and 'admin:org' scopes",
    )
    parser.add_argument(
        "--repo",
        help="Repository in format 'owner/repo' (e.g., 'lemonade-sdk/lemonade')",
    )
    parser.add_argument(
        "--org",
        help="Organization name to check org-level runners",
    )

    args = parser.parse_args()

    if not args.repo and not args.org:
        print("Error: At least one of --repo or --org must be specified", file=sys.stderr)
        sys.exit(1)

    checker = GitHubRunnerChecker(
        token=args.token,
        repo=args.repo,
        org=args.org,
    )

    try:
        checker.check_windows_runners()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
