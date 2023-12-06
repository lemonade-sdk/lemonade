# Release Notes

This document tracks the major changes in each package release of TurnkeyML.

We are tracking two types of major changes:
 - New features that enhance the user and developer experience
 - Breaking changes to the CLI or public APIs

If you are creating the release notes for a new version, please see the [template](#template-version-majorminorpatch). Release notes should capture all of the significant changes since the last numbered package release.

# Version 1.0.0

This version focuses on cleaning up technical debts and most of the changes are not visible to users. It removes cumbersome requirements for developers, removes unused features to streamline the codebase, and also clarifying some API naming schemes.

Users, however, will enjoy improved fidelity in their reporting telemetry thanks to the streamlined code.

## Users

### User Improvements

Improvements to the information in `turnkey_stats.yaml` and report CSVs:
 
 - Now reports all model labels. Including, but not limited to, the model's OSS license.
 - `build_status` and `benchmark_status` now accurately report the status of their respective toolchain phases.
     - Previously, `benchmark_status` was a superset of the status of both build and benchmark.

## User Breaking Changes

None.

## Developers

### Developer Improvements

 - Build success has been conceptually reworked for Stages/Sequences such that the `SetSuccess` Stage is no longer required at the end of every Sequence.
   - Previously, `build_model()` would only return a `State` object if the `state.build_status == successful_build`, which in turn had to be manually set in a Stage.
   - Now, if a Sequence finishes then the underlying toolflow will automatically set `state.build_status = successful_build` on your behalf.

### Developer Breaking Changes

 - The `benchmark_model()` API has been removed as there were no known users / use cases. Anyone who wants to run standalone benchmarking can still instantiate any `BaseRT` child class and call `BaseRT.benchmark()`.
 - The APIs for saving and loading labels `.txt` files in the cache have been removed since no code was using those APIs. Labels are now saved into `turnkey_stats.yaml` instead.
 - The `quantization_samples` argument to the `build_model()` API has been removed.
 - The naming scheme of the members of `Stats` has been adjusted for consistency. It used to refer to both builds and benchmarks as "builds", whereas now it uses "evaluations" as a superset of the two.
   - `Stats.add_build_stat()` is now `Stats.save_model_eval_stat()`.
   - `Stats.add_build_sub_stat()` is now `Stats.save_model_eval_sub_stat()`.
   - `Stats.stat_id` is now `Stats.evaluation_id`.
   - The `builds` section of the stats/reports is now `evaluations`.
   - `Stats.save_stat()` is now `Stats.save_model_stat()`.
   - `Stats.build_stats` is now `Stats.evaluation_stats`.
 - The `SetSuccess` build stage has been removed because build success has been reworked (see improvements).
 - The `logged_subprocess()` API has been moved from the `common.build` module to the `run.plugin_helpers` module.

# Version 0.3.0

This version was used to initialize the repository. 

# Template: Version Major.Minor.Patch

Headline statement.



## Users

### User Improvements

List of enhancements specific to users of the tools.

### User Breaking Changes

List of breaking changes specific to users of the tools.

## Developers

### Developer Improvements

List of enhancements specific to developers who build on the tools.

### Developer Breaking Changes

List of breaking changes specific to developers who build on the tools.
