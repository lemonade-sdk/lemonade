# Scripts

This directory contains utility scripts for managing and monitoring the Lemonade project.

## check_windows_runners.py

Check the status of Windows self-hosted GitHub Actions runners and see what jobs they're currently working on.

### Prerequisites

- Python 3.6 or higher (uses only standard library)
- GitHub Personal Access Token with appropriate permissions:
  - `repo` scope (to access repository runners and workflow runs)
  - `admin:org` scope (to access organization-level runners)

### Usage

Check repository-level runners:
```bash
python scripts/check_windows_runners.py --token YOUR_GITHUB_TOKEN --repo lemonade-sdk/lemonade
```

Check organization-level runners:
```bash
python scripts/check_windows_runners.py --token YOUR_GITHUB_TOKEN --org lemonade-sdk
```

Check both repository and organization runners:
```bash
python scripts/check_windows_runners.py --token YOUR_GITHUB_TOKEN --repo lemonade-sdk/lemonade --org lemonade-sdk
```

### Environment Variable

For convenience, you can set your GitHub token as an environment variable:

```bash
export GITHUB_TOKEN=your_token_here
python scripts/check_windows_runners.py --token $GITHUB_TOKEN --repo lemonade-sdk/lemonade
```

### Output

The script displays:
- Runner name
- Current status (online/offline)
- Whether the runner is busy
- Operating system
- Labels assigned to the runner
- If busy: Details about the current job including:
  - Job name
  - Workflow name
  - When it started
  - Link to the job

### Example Output

```
================================================================================
Windows Self-Hosted Runners Status
================================================================================

Fetching runners for repository: lemonade-sdk/lemonade
Found 3 Windows self-hosted runner(s)

Runner: windows-runner-01
  Status: online
  Busy: True
  OS: Windows
  Labels: self-hosted, windows, x64
  Currently working on:
  Job: Build and Test
  Workflow: C++ Server Build, Test, and Release 🚀
  Started: 2026-02-12T19:30:00Z
  URL: https://github.com/lemonade-sdk/lemonade/actions/runs/12345/jobs/67890

Runner: windows-runner-02
  Status: online
  Busy: False
  OS: Windows
  Labels: self-hosted, windows, x64
  Status: Idle

Runner: windows-runner-03
  Status: offline
  Busy: False
  OS: Windows
  Labels: self-hosted, windows, x64
  Status: Idle
```
