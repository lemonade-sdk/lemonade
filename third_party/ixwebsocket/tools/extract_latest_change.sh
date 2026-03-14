#!/bin/sh

grep -A 3 '^##' docs/CHANGELOG.md | head -n 3 | tail -n 1
