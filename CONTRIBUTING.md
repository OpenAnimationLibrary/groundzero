# Contributing to OAL Groundzero

Groundzero is Open Animation Library's environment for animation feature
exploration, integration, and testing. See [core planning](doc/core_planning.md)
for its relationship to OT-Dev and official OpenToonz and the current work areas.

## Groundzero proposals

1. Create a focused feature branch from `edition`, the current default and
   integration branch. Document any dependency on another proposal.
2. Open a PR in `OpenAnimationLibrary/groundzero` with base `edition`. Keep
   existing proposal branches and bases intact.
3. Preserve original authors, co-authors, and license notices. Follow the
   [development checklist](doc/development_checklist.md) and, where applicable,
   the [AI-assisted checklist](doc/ai_assisted_development_checklist.md).
4. Record the behavior changed, relevant checks, and outstanding application
   testing. Link related Groundzero, OT-Dev, and OpenToonz issues or PRs.
5. Select changes for integration individually after review and testing.

## Candidates for official OpenToonz

Extract the focused change onto current `opentoonz/opentoonz:master` and review
it through [OT-Dev](https://github.com/OpenAnimationLibrary/opentoonz-dev) using
[its contribution workflow](https://github.com/OpenAnimationLibrary/opentoonz-dev/blob/edition/CONTRIBUTING.md).
Keep Groundzero branding, OAL build infrastructure, unrelated experimental
changes, and CI test workflows out of the upstream patch. Retain contributor
history and keep the description brief.

Submitting a Groundzero PR does not submit anything to OT-Dev or official
OpenToonz. Existing PRs are not merged or retargeted as part of this planning
update. An upstream proposal is a separate, explicit action.

## Upstream reference

The remaining sections describe the inherited official OpenToonz process.
Use Groundzero's issue tracker for Groundzero-specific reports.

## Pull-requests

The OpenToonz organization loves any kind of contributions, such as fixing typos and code refactoring.
If you fixed or added something useful to OpenToonz, please send pull-requests to us.
We will first review the request, then we will accept it, add comments for rework, or decline it.

### Workflow

0. `fork` OpenToonz to your GitHub account from `opentoonz/opentoonz`.
  - (use the `fork` button at the https://github.com/opentoonz/opentoonz)
0. `clone` the repository.
  - `git clone git@github.com:your-github-account/opentoonz.git`
  - `git remote add upstream https://github.com/opentoonz/opentoonz.git`, additionally.
0. modify the codes.
  - `git checkout -b your-branch-name`
    - `your-branch-name` is a name of your modifications, for example,
      `fix/fatal-bugs`, `feature/new-useful-gui` and so on.
  - fix codes, then test them.
  - `git commit` them with good commit messages.
0. `pull` the latest changes form the `master` branch of the upstream.
  - `git pull upstream master` or `git pull --rebase upstream master`.
  - apply [clang-format](http://clang.llvm.org/docs/ClangFormat.html) with `toonz/sources/.clang-format`.
    - `cd toonz/sources`
    - `./beautification.sh` or `beautification.bat`.
  - `git commit` them.
  - `git push origin your-branch-name`.
0. make a pull request.

## Bugs

If you find bugs, please report details about them using [issues](https://github.com/opentoonz/opentoonz/issues).
Please include information needed to reproduce the bug, including the operating system 
and information directly relating to the issue. Links to screen captures of what is 
observed on screen or video of specific steps to produce the problem are very helpful.  
Then we will try to reproduce the bugs and fix them.
Unfortunately, bugs can sometimes only be reproduced in your own environment, 
so we cannot reproduce them. 
If you believe you can fix the bug, please submit a pull request.

## Features

If you had an idea about a new feature, please implement it and send a pull request to us.
If you cannot implement the feature, please open a topic on the [Google Group Page](https://groups.google.com/forum/#!forum/opentoonz_en).
It enables us to discuss implementations of the feature there.
Feature requests posted on GitHub without an active developer or funding will be closed 
to keep the issue tracker from becoming cluttered with unfulfilled feature requests.

## Translations

Translation sources (`.ts`) are in `toonz/sources/translations`. Contributors can
edit them with Qt Linguist and submit a PR, or attach the updated files to an
issue/discussion for a maintainer to integrate. A full application build is not
required to contribute wording. Share editable sources; compiled `.qm` files are
optional test artifacts unless a maintainer requests them.

See [Contributing and maintaining translations](toonz/sources/translations/README.md)
for the two workflows: sharing translations, and reviewing/merging/updating them.
The guide includes existing Qt Linguist, shell-script, and CMake/Visual Studio
processes, validation, runtime paths, and packaging checks.

## Keep personal data out

Don't put e-mail addresses (other than no-reply ones), private links such as AI
assistant session links, tokens, or local paths and hostnames in commits, pull
requests, issues or files. The `No PII` check fails pull requests that do; see
[CLAUDE.md](CLAUDE.md) for the details.
