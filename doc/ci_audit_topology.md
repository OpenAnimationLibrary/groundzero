# CI coverage and pull request check budget

The automatic pull request budget is ten job checks. The limit counts expanded
matrix jobs and jobs inside local reusable workflows, including jobs whose
conditions may skip them. It reserves checks for both source updates and later
metadata edits on the same commit. It does not count accumulated historical attempts or explicitly requested
manual runs.

The audit baseline is `2e52fb78b70915925393290f32dc09a95a3927ec`.
At that revision, 20 workflow files could produce 31 PR-event job checks when
all relevant path filters matched. Special proposal-branch push triggers could
start additional runs. The current `PR Checks` entry point expands the three
platform workflows and runs privacy scanning inside its `Required checks` summary. This produces ten
job checks across the PR source and metadata entry points: nine from `PR Checks`
and one reserved for the edited-event privacy scan. Metadata edits do not rebuild
the platforms.

The edition wrapper had added a summary, selector and automatic advisory calls
on top of the original ten jobs. [Run 43147442150][wrapper-run] materialized
fourteen checks while the budget incorrectly excluded those wrapper jobs. The guard now counts
all jobs, including conditional summaries and advisory calls, and rejects
unresolved matrices rather than omitting them.

## Automatic PR checks

| Entry point | Before | After | Checks after consolidation |
| --- | ---: | ---: | --- |
| Windows Build | 4 | 3 | Recorder inside application, Cadmium runtime, installed package |
| MacOS Build | 3 | 3 | Neural reference fixture, application, installed package |
| Linux Build | 2 | 2 | Application and consolidated suites, installed package |
| Required checks (including No PII) | 1 | 1 | Privacy policy and platform results |
| Edited-event No PII | 0 | 1 | Metadata scan reserved on the same commit |
| Neural style evaluation | 5 | 0 | Additional runtime matrix runs nightly and manually |
| SeExpr qualification | 8 | 0 | Additional SDK/backend matrix runs nightly, manually, and on edition |
| Plugin Manager | 2 | 0 | Both Qt versions run inside Linux Build |
| PureRef import | 1 | 0 | Import regression runs inside Linux Build |
| QSS Check | 2 | 0 | Comparison and review artifact run inside Linux Build |
| Docs — Production Journal | 2 | 0 | PR validation/build runs inside Linux Build; deployment remains separate |
| Release Changelog Checks | 1 | 0 | Release-note tests run inside Linux Build |
| **Total** | **31** | **10** | |

`Required checks` keeps its check name and runs even after a platform fails.
It scans the PR title, description, commit messages and added lines, then requires
both the privacy scan and all three platform workflow results to succeed. A failed
or skipped scan cannot produce a passing summary. Privacy writes remain scoped to
that job and share the existing per-PR concurrency group with edited-event scans.
This moves synchronization-event privacy reporting to the end of the platform jobs.

Automatic path selection and advisory fan-out are removed from `PR Checks`.
The existing scheduled/manual neural and SeExpr workflows remain available;
SeExpr also retains post-merge edition qualification. Native neural integration,
SeExpr integration and Linux sanitizer checks remain automatic PR gates.

The former documentation workflow included a deployment job skipped on PRs.
It is counted conservatively above. Reusable workflow call wrappers are expanded
into their actual jobs rather than counted as extra jobs themselves. The separate
metadata scan is conservatively reserved even when no edit occurs.

## Preserved coverage

The recorder encoder and its test-only decoder are built or restored inside the
Windows application job on the same `windows-2025` image. The same source recipe,
smoke test, cache key and uploaded artifacts are retained; local files replace
the former same-run artifact download. An explicit save preserves a successful
recorder cache even if a later application step fails. The MSYS2 action only adds
its shell-shim directory to PATH, and its `MSYSTEM` setting is restored before
application steps. A cold-cache recorder build now runs after Cadmium and before
the application, so elapsed Windows job time can increase. The installed-package
check continues to use a fresh runner.

Linux Build runs plugin-manager regressions with Qt 5 and Qt 6 in separate steps.
It also runs the PureRef import regression with Qt 5, temporal motion and
occlusion tests, and the SeExpr core/parser corpus under address and undefined
behavior sanitizers. The sanitizer environment, CTest failure behavior and test
timeout match the standalone qualification. These suites retain build/test logs,
JUnit results where provided, and CTest failure logs. Their failure annotations
use the existing bounded diagnostic reporter.

The consolidated suites run after application packaging. Each suite checks its
own dependency readiness and still runs after an unrelated failure. A Qt 5
failure therefore does not skip Qt 6, and a stylesheet failure does not prevent
native application build/test execution. A failed step still fails Linux Build.
An unsuccessful Linux Build can prevent its dependent installed-package job,
which remains part of the ten-check budget.

Stylesheet comparison runs before generation. Stale committed QSS remains a
failure even though corrected review artifacts are subsequently generated.
Release-note tests, reference-feed validation, and the single-file wiki build
retain their validation and build behavior. Less and TiddlyWiki use the committed
toolchain lock with package scripts disabled. CI budget tests and CI failure-classification
and pagination tests run in the Linux job as well.

The existing Linux job also runs checksum-verified actionlint and offline zizmor,
with independent failure conditions and retained logs. No additional job check is
created for these static tools. The platform configurations enable the deformer
and Auto-Patch fixtures and require them in the named regression lists. Native
plugin and external-process fixtures are registered in those lists as well.
The public FFmpeg fixture carries the new HDR source-preservation assertions on
all platforms; the separate direct-class suite remains Linux/macOS-only.

Native neural-host integration remains in all three application workflows.
The extra LiteRT runtime matrix, including macOS ARM64, and the standalone
SeExpr interpreter, LLVM and Python/PyQt SDK matrices move from PR gates to
nightly/manual qualification. This changes when those additional configurations
are qualified; it does not make them PR prerequisites. The standalone SeExpr
workflow also retains post-merge edition qualification. PRs keep Linux SeExpr
sanitizers and each platform's native application and installed-package checks.

Linux Tests gains nightly execution of its full registered CTest suite,
compatibility comparison, and Qt 6 build/test jobs. Native process contracts gain
a nightly run while retaining manual dispatch. Existing native-process coverage
inside platform jobs remains. The inherited OT-Dev preservation workflow becomes
manual only; its existing repository condition remains in place.

## Budget enforcement

Run the read-only topology check with:

```sh
ruby ci-scripts/check_ci_budget.rb
```

It fails above ten checks, expands nested local reusable workflows, handles
Cartesian matrices plus `include` and `exclude`, and rejects unresolved dynamic
matrices, external reusable workflows, and cycles. Wrapper summary and selector
jobs count like other jobs. Separate workflows with disjoint PR activity types are added together so a later description edit cannot
silently exceed the same-commit budget. Both `pull_request` and
`pull_request_target` are counted; configuring both counts both event runs.
Matrix semantics follow the
[GitHub matrix documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/run-job-variations).

Proposal-branch push triggers are removed because a push and a PR update can
otherwise start additional checks for the same revision. The guard permits
explicit post-merge push coverage only for the integration branches `edition`
and `main`, plus tag-only pushes. The former SeExpr proposal-branch exception
is removed; use manual dispatch to qualify such a branch without duplicate PR
checks. It rejects unbounded/other branch push filters and indirect `workflow_run`, `check_run`, or `check_suite` triggers whose PR
impact is not statically modeled. Integration branches are not proposal source
branches for this policy. Schedules and manual dispatch remain outside the PR
budget. Changing the integration-branch policy requires updating the guard and
this document together.

## Verification limits

The baseline and revised topology were inspected statically. The revised guard
reports `Automatic PR checks: 10/10`. The wrapper-counting, unresolved-advisory
matrix and proposal-branch regression cases are authored; execution is pending
CI. The current source repair passes Ruby syntax and whitespace validation,
actionlint 1.7.12 with ShellCheck for the three changed workflows, and zizmor 1.32.0
offline for those workflows (25 ignored and three suppressed, with no unsuppressed
findings). These results do not establish runtime qualification. No application
builds, application tests, or guard regression tests were run locally: repository policy reserves builds and tests for CI.

CI still needs to demonstrate successful combined execution, diagnostic artifact
retention, runner dependency availability, and sufficient time within the Linux
job's configured 240-minute timeout. The additional nightly matrices must also run
successfully before their separate configuration coverage is claimed. The exact `Required checks` summary name is preserved. Repository settings are
not changed by this patch. An authenticated read found no classic protection on
`edition`; the visible active ruleset only prohibited non-fast-forward updates.
That snapshot does not authorize merging or establish future repository policy.

[wrapper-run]: https://github.com/OpenAnimationLibrary/groundzero/actions/runs/43147442150
