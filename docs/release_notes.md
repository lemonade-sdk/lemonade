# Release Notes

This document tracks the major changes in each package release of TurnkeyML.

We are tracking two types of major changes:
 - New features that enhance the user and developer experience
 - Breaking changes to the CLI or public APIs

If you are creating the release notes for a new version, please see the [template](#template-version-majorminorpatch). Release notes should capture all of the significant changes since the last numbered package release.

# Version 1.1.1

This is a small release that:
- Adds hundreds of new Transformer models, bringing the `models/transformers` corpus from 78 to 289 total models.
- Changes the pytorch ONNX export validation criteria from their default tolerances (which was very strict) to instead match the tolerances recommended by PyTorch.


# Version 1.1.0

This version focuses on improving the clarity of the telemetry reported.

## Users

### User Improvements

- ONNX files exported from PyTorch models now have a `torch_export_validity` key in their stats/report files that indicates whether the `torch.onnx.verification.find_mismatch()` API could find any issue with the exported ONNX file. Possible values:
  - `valid`: passed verification.
  - `invalid`: failed verification.
  - `unverified`: turnkey was unable to complete the verification process.
- Stats and report CSV files split `stages_completed` into stage status and duration.
- Build, benchmark, and stage status values in the stat and report files now use the same terminology values:

```
class FunctionStatus(enum.Enum):
    """
    Status values that are assigned to stages, builds, benchmarks, and other
    functionality to help the user understand whether that function completed
    successfully or not.
    """

    # SUCCESSFUL means the stage/build/benchmark completed successfully.
    SUCCESSFUL = "successful"

    # ERROR means the stage/build/benchmark failed and threw some error that
    # was caught by turnkey. You should proceed by looking at the build
    # logs to see what happened.

    ERROR = "error"

    # KILLED means the build/benchmark failed because the system killed it. This can
    # happen because of an out-of-memory (OOM), timeout, system shutdown, etc.
    # You should proceed by re-running the build and keeping an eye on it to observe
    # why it is being killed (e.g., watch the RAM utilization to diagnose an OOM).
    KILLED = "killed"

    # The NOT_STARTED status is applied to all stages/builds/benchmarks at startup.
    # It will be replaced by one of the other status values if the stage/build/benchmark
    # has a chance to start running.
    # A value of NOT_STARTED in the report CSV indicates that the stage/build/benchmark
    # never had a chance to start because turnkey exited before that functionality had
    # a chance to start running.
    NOT_STARTED = "not_started"

    # INCOMPLETE indicates that a stage/build/benchmark started running and did not complete.
    # Each stage, build, and benchmark are marked as INCOMPLETE when they start running.
    # If you open the turnkey_stats.yaml file while the stage/build/benchmark
    # is still running, the status will show as INCOMPLETE. If the stage/build/benchmark
    # is killed without the chance to do any stats cleanup, the status will continue to
    # show as INCOMPLETE in turnkey_stats.yaml.
    # When the report CSV is created, any instance of an INCOMPLETE stage/build/benchmark
    # status will be replaced by KILLED.
    INCOMPLETE = "incomplete"
```

- The CLI help page for the `benchmark` command has been reorganized for clarity (try `turnkey benchmark -h`).
- The CLI now provides more helpful errors when the user provides arguments incorrectly.
- Fixed a bug where multi-cache reporting could repeat entries in the report CSV file.


## User Breaking Changes

- Previous turnkey caches are not compatible with this version and must be rebuilt.
- The status terminology changes documented above mean that stats/reports from pre-v1.1.0 builds are not directly comparable to post-v1.1.0 builds.

## Developers

### Developer Improvements

None

### Developer Breaking Changes

- `build.Status` and `filesystem.FunctionStatus` have both been removed, and replaced with `build.FunctionStatus` which is the union of those two Enums.

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
