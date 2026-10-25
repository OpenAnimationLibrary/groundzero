# Open Animation Library core planning

Groundzero is part of Open Animation Library (OAL) development planning alongside
OT-Dev and official OpenToonz. Each repository has a distinct role:

| Repository | Role |
| --- | --- |
| [Groundzero](https://github.com/OpenAnimationLibrary/groundzero) | Explore and validate features together in an OAL development environment. |
| [OT-Dev](https://github.com/OpenAnimationLibrary/opentoonz-dev) | Review and stage focused OpenToonz proposals, with their testing and contributor history. |
| [OpenToonz](https://github.com/opentoonz/opentoonz) | Official upstream review, acceptance, and releases. |

## Working rules

- Groundzero's current integration and default branch is `edition`. Start new
  Groundzero work there unless the feature requires a documented dependency.
- Keep existing PR heads and bases intact. Opening this planning work does not
  merge or retarget the pending feature proposals.
- Link related Groundzero, OT-Dev, and upstream work before implementing or
  porting a feature. Record dependencies explicitly and avoid duplicate fixes.
- Validate each change independently before considering integration. Record
  compilation, automated checks, and application testing separately; an open PR
  or successful build does not establish that an artist workflow works.
- When a Groundzero change is selected for OpenToonz, extract the focused change
  onto the current official upstream base, retaining authors and co-authors.
  Review it through OT-Dev before explicitly submitting upstream. Leave OAL
  branding, packaging infrastructure, and experimental dependencies here.
- Track proposals in their repository's issues and PRs. Cross-link their status;
  acceptance in Groundzero does not imply acceptance in OT-Dev or OpenToonz.
- Keep descriptions brief and direct. Use the existing development and
  AI-assisted checklists for implementation and review.

## Current work

This inventory records the open proposals reviewed on October 24, 2026.
Follow the linked PRs for current status, dependencies, checks, and limitations.
The groupings describe work areas, not merge order or completion commitments.

| Work area | Groundzero proposals |
| --- | --- |
| Reliability and test coverage | [Tool helper tests](https://github.com/OpenAnimationLibrary/groundzero/pull/1), [paste undo balance](https://github.com/OpenAnimationLibrary/groundzero/pull/2), [shared test switch](https://github.com/OpenAnimationLibrary/groundzero/pull/9), [FX render regression](https://github.com/OpenAnimationLibrary/groundzero/pull/10), [Unix crash reports](https://github.com/OpenAnimationLibrary/groundzero/pull/13), [undo memory accounting](https://github.com/OpenAnimationLibrary/groundzero/pull/15) |
| Platform modernization | [Qt screen APIs](https://github.com/OpenAnimationLibrary/groundzero/pull/3) |
| Animation and drawing | [Selection lock](https://github.com/OpenAnimationLibrary/groundzero/pull/4), [ease presets](https://github.com/OpenAnimationLibrary/groundzero/pull/5), [breakdown keys](https://github.com/OpenAnimationLibrary/groundzero/pull/11), [Light Table](https://github.com/OpenAnimationLibrary/groundzero/pull/12), [Plastic IK](https://github.com/OpenAnimationLibrary/groundzero/pull/17), [thickness brushes](https://github.com/OpenAnimationLibrary/groundzero/pull/20), [trajectories](https://github.com/OpenAnimationLibrary/groundzero/pull/21), [Plastic bone attachments](https://github.com/OpenAnimationLibrary/groundzero/pull/22) |
| Audio | [Soundtrack meter](https://github.com/OpenAnimationLibrary/groundzero/pull/6), [animated sound volume](https://github.com/OpenAnimationLibrary/groundzero/pull/19) |
| FX, interchange, and performance | [GLB cache](https://github.com/OpenAnimationLibrary/groundzero/pull/14), [PSD export](https://github.com/OpenAnimationLibrary/groundzero/pull/16), [Tone / Highlight FX](https://github.com/OpenAnimationLibrary/groundzero/pull/18), [FFmpeg pipe export](https://github.com/OpenAnimationLibrary/groundzero/pull/23), [single-frame parallel rendering](https://github.com/OpenAnimationLibrary/groundzero/pull/24) |

Prioritize a reproducible build and regression baseline, then validate the
individual workflows. Rendering and export proposals need output comparisons,
error/cancellation checks, and representative Windows testing before relying on
their performance gains. Rigging proposals need save/reload, undo, and existing
scene checks. Select any upstream candidates individually after that evidence.

## Identity

- Organization: **Open Animation Library (OAL)**.
- Application and repository: **Groundzero**; display name **OAL Groundzero**.
- Keep OpenToonz ancestry, license notices, and contributor credit accurate.
- Keep internal executable names, configuration keys, file formats, and build
  identifiers compatible during this branding pass. Those technical names are
  not a product identity and need a separate migration if changed later.
- Groundzero Windows packages use the existing replaceable
  `portablestuff/splash.png`, stable SVG template, build metadata, and opt-in
  session recorder. See [Windows branding](../.github/ot-dev/BRANDING.md).
