# Groundzero

**Open Animation Library (OAL)** · Experimental animation development based on
OpenToonz.

Groundzero brings feature exploration, integration, and testing into OAL's core
planning alongside [OT-Dev](https://github.com/OpenAnimationLibrary/opentoonz-dev)
and [official OpenToonz](https://github.com/opentoonz/opentoonz). Groundzero is
maintained independently by Open Animation Library.

## Planning and development

- [Core planning and current work](doc/core_planning.md)
- [Contributing](CONTRIBUTING.md)
- [Development checklist](doc/development_checklist.md)
- [AI-assisted development checklist](doc/ai_assisted_development_checklist.md)
- [Open proposals](https://github.com/OpenAnimationLibrary/groundzero/pulls)
- [Contributors](https://github.com/OpenAnimationLibrary/groundzero/issues/8)

The default integration branch is `edition`. Features under review are separate
proposals; check each PR's tests and application-testing notes before using it.
Upstream submission is an explicit step after review through OT-Dev.

## Builds

[Groundzero Actions](https://github.com/OpenAnimationLibrary/groundzero/actions)
provides build runs and available artifacts. Windows CI packages identify the
Groundzero PR or branch, source commit, and build date. Replace
`portablestuff/splash.png` to customize the artwork without changing the SVG.
See [Windows build branding](.github/ot-dev/BRANDING.md) for package details.

Local build instructions inherited from OpenToonz:
[Windows](doc/how_to_build_win.md), [macOS](doc/how_to_build_macosx.md),
[Linux](doc/how_to_build_linux.md), and [BSD](doc/how_to_build_bsd.md).
Technical target and executable names still use OpenToonz.

## OpenToonz ancestry

OpenToonz is a 2D animation application published by [DWANGO](https://dwango.co.jp/).
It is based on Toonz Studio Ghibli Version, originally developed by Digital Video
and customized by Studio Ghibli. Groundzero retains that source ancestry,
licensing, and contributor history.

For official OpenToonz downloads and documentation, use
[opentoonz.github.io](https://opentoonz.github.io/) and
[opentoonz/opentoonz](https://github.com/opentoonz/opentoonz).
Report Groundzero-specific problems in
[this repository](https://github.com/OpenAnimationLibrary/groundzero/issues).

## Licensing

- Files outside of the `thirdparty` and `stuff/library/mypaint brushes` directories are based on the Modified BSD License.
  - [modified BSD license](./LICENSE.txt).
  - Based on this license, this software may be used or changed freely for business or personal use.
- For files in the `thirdparty` directory:
  - Please consult with the licenses in the appropriate READMEs or source codes.
- For files in the `stuff/library/mypaint brushes` directory:
  - Please see the licenses in `stuff/library/mypaint brushes/Licenses.txt`.

### Special Thanks

This open-source program is developed from Toonz, a software originally created by Digital Video, S.p.A., Rome, Italy

## Contributing translations

To share updated translation files or review, merge, and refresh catalogs, see
[Contributing and maintaining translations](toonz/sources/translations/README.md).
Qt Linguist editing and existing manual translation tools remain supported.
