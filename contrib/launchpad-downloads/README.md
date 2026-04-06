# Launchpad PPA Download Statistics Tool

A Python tool to retrieve download statistics for packages in Launchpad PPAs (Personal Package Archives).

## Installation

1. Install dependencies:
```bash
pip install -r requirements.txt
```

Or install directly:
```bash
pip install launchpadlib
```

## Usage

Basic usage:
```bash
python ppa_stats.py username/ppa-name
```

### Examples

Get stats for all packages in a PPA:
```bash
python ppa_stats.py developmentseed/mapbox
```

Get stats for a specific package:
```bash
python ppa_stats.py developmentseed/mapbox -p tilemill
```

Show all versions including those with 0 downloads:
```bash
python ppa_stats.py username/ppa-name --all
```

Show summary grouped by package name:
```bash
python ppa_stats.py username/ppa-name --summary
```

### Options

- `-p, --package PACKAGE` - Query a specific package name
- `--all` - Show all versions including those with 0 downloads
- `--summary` - Show summary by package name instead of individual versions

## How It Works

The tool uses the Launchpad API via the `launchpadlib` Python library to:

1. Connect anonymously to Launchpad's production API
2. Retrieve the specified PPA for the given user/team
3. Fetch published binary packages and their download counts
4. Display formatted statistics

## API Documentation

The tool uses Launchpad's public API:
- API endpoint: https://api.launchpad.net/
- No authentication required for read-only access
- Uses the `getDownloadCount()` method on published binaries

## Output Format

The tool displays:
- Total downloads across all packages/versions
- Individual package versions with download counts
- Architecture information for each binary package
- Sorted by download count (most popular first)

## Notes

- Download statistics are provided by Launchpad and may have some delay
- The tool caches API responses in `~/.launchpadlib/cache/` for performance
- Anonymous access is sufficient for read-only statistics

## Sources

Based on information from:
- [Launchpad API Documentation](https://api.launchpad.net/)
- [PPA Download Statistics Examples](https://gist.github.com/springmeyer/2778600)
- [ppa-stats GitHub Project](https://github.com/hsheth2/ppa-stats)
