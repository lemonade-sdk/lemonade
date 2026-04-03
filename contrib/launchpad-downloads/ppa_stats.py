#!/usr/bin/env python3
"""
Launchpad PPA Download Statistics Tool

This tool retrieves download statistics for packages in a Launchpad PPA.
"""

import os
import sys
import argparse
from launchpadlib.launchpad import Launchpad


def get_ppa_stats(username, ppa_name, package_name=None, show_all=False):
    """
    Get download statistics for a Launchpad PPA.

    Args:
        username: Launchpad username/team name
        ppa_name: Name of the PPA
        package_name: Optional specific package name to query
        show_all: Show all versions including those with 0 downloads

    Returns:
        Total downloads and list of package statistics
    """
    # Setup cache directory
    cachedir = os.path.expanduser("~/.launchpadlib/cache/")

    print(f"Connecting to Launchpad API...")
    launchpad = Launchpad.login_anonymously(
        "ppa-stats-tool", "production", cachedir, version="devel"
    )

    # Get the PPA
    try:
        print(f"Fetching PPA: {username}/{ppa_name}")
        ppa = launchpad.people[username].getPPAByName(name=ppa_name)
    except Exception as e:
        print(f"Error: Could not find PPA '{ppa_name}' for user '{username}'")
        print(f"Details: {e}")
        sys.exit(1)

    # Get published binaries
    if package_name:
        print(f"Fetching statistics for package: {package_name}")
        bins = ppa.getPublishedBinaries(binary_name=package_name)
    else:
        print(f"Fetching statistics for all packages...")
        bins = ppa.getPublishedBinaries()

    # Collect download counts
    builds = []
    total_downloads = 0

    for binary in bins:
        count = binary.getDownloadCount()
        total_downloads += count

        if count > 0 or show_all:
            builds.append(
                {
                    "count": count,
                    "name": binary.binary_package_name,
                    "version": binary.binary_package_version,
                    "arch": binary.distro_arch_series_link.split("/")[-1],
                }
            )

    # Sort by download count (descending)
    builds_sorted = sorted(builds, key=lambda x: x["count"], reverse=True)

    return total_downloads, builds_sorted


def print_stats(total_downloads, builds, show_details=True):
    """Print download statistics in a formatted way."""
    print("\n" + "=" * 70)
    print(f"TOTAL DOWNLOADS: {total_downloads:,}")
    print("=" * 70)

    if not builds:
        print("No packages found or no downloads recorded.")
        return

    if show_details:
        print(f"\n{'Downloads':<12} {'Package':<30} {'Version':<20} {'Arch':<10}")
        print("-" * 70)

        for build in builds:
            print(
                f"{build['count']:<12,} {build['name']:<30} {build['version']:<20} {build['arch']:<10}"
            )
    else:
        # Group by package name
        package_totals = {}
        for build in builds:
            name = build["name"]
            if name not in package_totals:
                package_totals[name] = 0
            package_totals[name] += build["count"]

        print(f"\n{'Downloads':<12} {'Package':<30}")
        print("-" * 42)

        for name, count in sorted(
            package_totals.items(), key=lambda x: x[1], reverse=True
        ):
            print(f"{count:<12,} {name:<30}")


def main():
    parser = argparse.ArgumentParser(
        description="Get download statistics for a Launchpad PPA",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s username/ppa-name
  %(prog)s developmentseed/mapbox -p tilemill
  %(prog)s myuser/myppa --all --summary
        """,
    )

    parser.add_argument("ppa", help="PPA in format: username/ppa-name")
    parser.add_argument(
        "-p", "--package", help="Specific package name to query (optional)"
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Show all versions including those with 0 downloads",
    )
    parser.add_argument(
        "--summary",
        action="store_true",
        help="Show summary by package name instead of individual versions",
    )

    args = parser.parse_args()

    # Parse PPA format
    if "/" not in args.ppa:
        print("Error: PPA must be in format 'username/ppa-name'")
        sys.exit(1)

    username, ppa_name = args.ppa.split("/", 1)

    try:
        total, builds = get_ppa_stats(username, ppa_name, args.package, args.all)

        print_stats(total, builds, show_details=not args.summary)

    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        sys.exit(130)
    except Exception as e:
        print(f"\nError: {e}")
        import traceback

        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
