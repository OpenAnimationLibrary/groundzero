# CI coverage and pull request check budget

The automatic pull request budget is ten job checks. The limit counts expanded
matrix jobs and jobs inside local reusable workflows, including jobs whose
conditions may skip them. It covers each automatic PR update, not accumulated
historical runs or explicitly requested manual runs on the same commit.

The audit baseline is `2e52fb78b70915925393290f32dc09a95a3927ec`.
At that revision, 20 workflow files could produce 31 PR-event job checks when
all relevant path filters matched. Special proposal-branch push triggers could
start additional runs. The revised layout has 16 workflow files and four PR
entry points producing at most ten job checks.

## Automatic PR checks

| Entry point | Before | After | Checks after consolidation |
| --- | ---: | ---: | --- |
| Windows Build | 4 | 4 | Recorder encoder, Cadmium runtime, application, installed package |
| MacOS Build | 3 | 3 | Neural reference fixture, application, installed package |
| Linux Build | 2 | 2 | Application and consolidated suites, installed package |
| No PII | 1 | 1 | Repository privacy policy |
| Neural style evaluation | 5 | 0 | Additional runtime matrix runs nightly and manually |
| SeExpr qualification | 8 | 0 | Additional SDK/backend matrix runs nightly, manually, and on edition |
| Plugin Manager | 2 | 0 | Both Qt versions run inside Linux Build |
| PureRef import | 1 | 0 | Import regression runs inside Linux Build |
| QSS Check | 2 | 0 | Comparison and review artifact run inside Linux Build |
| Docs — Production Journal | 2 | 0 | PR validation/build runs inside Linux Build; deployment remains separate |
| Release Changelog Checks | 1 | 0 | Release-note tests run inside Linux Build |
| **Total** | **31** | **10** | |

The former documentation workflow included a deployment job skipped on PRs.
It is counted conservatively above. Reusable workflow call wrappers are expanded
into their actual jobs rather than counted as extra jobs themselves.

## Preserved coverage

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
matrices, external reusable workflows, and cycles. Both `pull_request` and
`pull_request_target` are counted; configuring both counts both event runs.
Matrix semantics follow the
[GitHub matrix documentation](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/run-job-variations).

Proposal-branch push triggers are removed because a push and a PR update can
otherwise start additional checks for the same revision. The guard permits
explicit post-merge push coverage only for the integration branches `edition`
and `main`, plus tag-only pushes. It rejects unbounded/other branch push filters
and indirect `workflow_run`, `check_run`, or `check_suite` triggers whose PR
impact is not statically modeled. Integration branches are not proposal source
branches for this policy. Schedules and manual dispatch remain outside the PR
budget. Changing the integration-branch policy requires updating the guard and
this document together.

## Verification limits

The baseline and revised topology were inspected statically. The revised guard
reports `Automatic PR checks: 10/10`. Ruby syntax validation, actionlint with
ShellCheck for the changed workflows, and whitespace validation passed during
preparation. No application builds, application tests, or guard regression tests
were run locally: repository policy reserves builds and tests for CI.

CI still needs to demonstrate successful combined execution, diagnostic artifact
retention, runner dependency availability, and sufficient time within the Linux
job's configured 240-minute timeout. The additional nightly matrices must also run
successfully before their separate configuration coverage is claimed. Required
check names in repository branch protection must be reviewed when retiring the
standalone PR workflows; repository settings are not changed by this patch.
