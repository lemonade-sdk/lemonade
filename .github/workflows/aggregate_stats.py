import json
import os
import pathlib
from tabulate import tabulate

rows = []
artifacts_path = pathlib.Path('artifacts')
for p in artifacts_path.glob('stats-*'):
    try:
        data = json.load(open(p / 'stats.json'))
    except FileNotFoundError:
        continue
    label = p.name[6:]
    if label.startswith('ubuntu-latest-'):
        os_ = 'ubuntu-latest'
        scenario = label[len('ubuntu-latest-'):]
    elif label.startswith('windows-latest-'):
        os_ = 'windows-latest'
        scenario = label[len('windows-latest-'):]
    else:
        os_, scenario = label.split('-', 1)
    rows.append(
        (
            os_,
            scenario,
            data['size'] / 1024 / 1024,
            data['time'],
            data.get('zip', 0) / 1024 / 1024,
        )
    )

if rows:
    table = [
        (os_, scenario, f"{size:.2f}", time, f"{zip_size:.2f}")
        for os_, scenario, size, time, zip_size in rows
    ]
    print(
        tabulate(
            table,
            headers=['OS', 'Scenario', 'Size (MB)', 'Install Time (s)', 'Zip Size (MB)'],
            tablefmt='github',
        )
    )
else:
    print('No artifacts to summarize.')
