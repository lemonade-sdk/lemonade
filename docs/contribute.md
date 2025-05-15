# Lemonade SDK Contribution Guide

Hello and welcome to the project! 🎉

We're thrilled that you're considering contributing to the project. This project is a collaborative effort and we welcome contributors from everyone.

Before you start, please take a few moments to read through these guidelines. They are designed to make the contribution process easy and effective for everyone involved. Also take a look at the [code organization](https://github.com/lemonade-sdk/lemonade/blob/main/docs/code.md) for a bird's eye view of the repository.

The guidelines document is organized as the following sections:
- [Contributing a Lemonade Server Demo](#-contributing-a-lemonade-server-demo)
- [Contributing to the overall framework](#contributing-to-the-overall-framework)
- [Issues](#issues)
- [Pull Requests](#pull-requests)
- [Testing](#testing)
- [Versioning](#versioning)
- [PyPI Release Process](#pypi-release-process)

## 🍋 Contributing a Lemonade Server Demo

Lemonade Server Demos aim to be reproducible in under 10 minutes, require no code changes to the app you're integrating, and use an app supporting the OpenAI API with a configurable base URL. 

Please see [AI Toolkit ReadMe](./server/apps/ai-toolkit.md) for an example Markdown contribution.

- To Submit your example, open a pull request in the GitHub repo with the following:
  - Add your .md file in the [Server apps](https://github.com/lemonade-sdk/lemonade/tree/main/docs/server/apps) folder.
  - Assign your PR to the maintainers

We’re excited to see what you build! If you’re unsure about your idea or need help unblocking an integration, feel free to reach out via GitHub Issues or [email](mailto:lemonade@amd.com).

## Contributing to the overall framework
If you wish to contribute to any other part of the repository such as examples or reporting, please open an [issue](#issues) with the following details.

1. **Title:** A concise, descriptive title that summarizes the contribution.
1. **Tags/Labels:** Add any relevant tags or labels such as 'enhancement', 'good first issue', or 'help wanted'
1. **Proposal:** Detailed description of what you propose to contribute. For new examples, describe what they will demonstrate, the technology or tools they'll use, and how they'll be structured.

## Issues

Please file any bugs or feature requests you have as an [Issue](https://github.com/lemonade-sdk/lemonade/issues) and we will take a look.

## Pull Requests

Contribute code by creating a pull request (PR). Your PR will be reviewed by one of the [repo maintainers](https://github.com/lemonade-sdk/lemonade/blob/main/CODEOWNERS).

Please have a discussion with the team before making major changes. The best way to start such a discussion is to file an [Issue](https://github.com/lemonade-sdk/lemonade/issues) and seek a response from one of the [repo maintainers](https://github.com/lemonade-sdk/lemonade/blob/main/CODEOWNERS).

## Testing

Tests are defined in `tests/` and run automatically on each PR, as defined in our [testing action](https://github.com/lemonade-sdk/lemonade/blob/main/.github/workflows/test.yml). This action performs both linting and unit testing and must succeed before code can be merged.

We don't have any fancy testing framework set up yet. If you want to run tests locally:
- Activate a `conda` environment that has `lemonade-sdk` (this package) installed.
- Run `conda install pylint` if you haven't already (other pylint installs will give you a lot of import warnings).
- Run `pylint src --rcfile .pylintrc` from the repo root.
- Run `python *.py` for each test script in `test/`.

## Versioning

We use semantic versioning, as described in [versioning.md](https://github.com/lemonade-sdk/lemonade/blob/main/docs/versioning.md).

## PyPI Release Process

Lemonade SDK is provided as a package on PyPI, the Python Package Index, as [lemonade-sdk](https://pypi.org/project/lemonade-sdk/). The release process for pushing an updated package to PyPI is mostly automated, however (by design), there are a few manual steps.
1. Make sure the version number in [version.py](https://github.com/lemonade-sdk/lemonade/blob/main/src/lemonade/version.py) has a higher value than the current [PyPI package](https://pypi.org/project/lemonade-sdk/).
    - Note: if you don't take care of this, PyPI will reject the updated package and you will need to start over from Step 1 of this guide.
    - If you are unsure what to set the version number to, consult [versioning.md](https://github.com/lemonade-sdk/lemonade/blob/main/docs/versioning.md).
1. Make sure all of the changes you want to release have been merged to `main`.
1. Go to the [Lemonade SDK GitHub front page](https://github.com/lemonade-sdk/lemonade) and click "Releases" in the side bar.
1. At the top of the page, click "Draft a new release".
1. Click "Choose a tag" (near the top of the page) and write `v` (lowercase), followed by the contents of the string in [version.py](https://github.com/onnx/lemonade-sdk/lemonade/main/src/lemonade/version.py).
  - For example, if `version.py` contains `__version__ = "4.0.5"`, the string is `4.0.5` and you should write `v4.0.5` into the text box.
1. Click the "+Create new tag:... on publish" button that appears under the next box.
1. Click "Generate release notes" (near the top of the page). Modify as necessary. Make sure to give credit where credit is due!
1. Click "Publish release" (green button near the bottom of the page). This will start the release automations, in the form of a [Publish Distributions to PyPI Action](https://github.com/lemonade-sdk/lemonade/actions/workflows/publish-to-test-pypi.yml).
  - Note: if you forgot the "v" in the "Choose a tag" step, this Action will not run.
1. Wait for the Action launched by the prior step to complete. Go to [the lemonade-sdk PyPI page](https://pypi.org/project/lemonade-sdk/) and spam refresh. You should see the version number update.
  - Note: `pip install lemonade-sdk` may not find the new update for a few more minutes.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->