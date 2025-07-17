# Versioning Policy

The `lemonade-sdk` package applies semantic versioning for its 3-digit version number. The version number is stored in `src/version.py`.

The 3 digits correspond to MAJOR.MINOR.PATCH, which can be interpreted as follows:
* MAJOR: changes indicate breaking API changes that may require the user to change their own code
* MINOR: changes indicate that builds against a previous minor version may not be compatible, and the user may need to rebuild those models
* PATCH: no user action required when the patch number changes

## PyPI Release Process

Lemonade SDK is provided as a package on PyPI, the Python Package Index, as [lemonade-sdk](https://pypi.org/project/lemonade-sdk/). The release process for pushing an updated package to PyPI is mostly automated, however (by design), there are a few manual steps.

1. Make sure the version number in [version.py](https://github.com/lemonade-sdk/lemonade/blob/main/src/lemonade/version.py) has a higher value than the current [PyPI package](https://pypi.org/project/lemonade-sdk/).
    - Note: if you don't take care of this, PyPI will reject the updated package and you will need to start over from Step 1 of this guide.
    - If you are unsure what to set the version number to, consult the section above.
1. Make sure all of the changes you want to release have been merged to `main`.
1. Go to the [Lemonade SDK GitHub front page](https://github.com/lemonade-sdk/lemonade) and click "Releases" in the side bar.
1. At the top of the page, click "Draft a new release".
1. Click "Choose a tag" (near the top of the page) and write `v` (lowercase), followed by the contents of the string in [version.py](https://github.com/onnx/lemonade-sdk/lemonade/main/src/lemonade/version.py).
  - For example, if `version.py` contains `__version__ = "4.0.5"`, the string is `4.0.5` and you should write `v4.0.5` into the text box.
  - Note: This is not optional because the release action only runs if the tag name starts with `v`.
1. Click the "+Create new tag:... on publish" button that appears under the next box.
1. Click "Generate release notes" (near the top of the page). Modify as necessary. Make sure to give credit where credit is due!
1. Click "Publish release" (green button near the bottom of the page). This will start the release automations, in the form of a [Publish Distributions to PyPI Action](https://github.com/lemonade-sdk/lemonade/actions/workflows/publish-to-test-pypi.yml).
  - Note: if you forgot the "v" in the "Choose a tag" step, this Action will not run.
1. Wait for the Action launched by the prior step to complete. Go to [the lemonade-sdk PyPI page](https://pypi.org/project/lemonade-sdk/) and spam refresh. You should see the version number update.
  - Note: `pip install lemonade-sdk` may not find the new update for a few more minutes.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->