# App info backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

(none)

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | Version and build facts generated from the build system | CMake `configure_file`s a header out of `project(VERSION/DESCRIPTION/HOMEPAGE_URL)` + the git commit + the compiler/Qt version; `main.cpp` no longer writes `"1.0.0"` | before the first external release (the version number has to reach the installer and bug reports) | `../design/appfw-app-info.md` F5 |
| T2 | Manifest file + third-party acknowledgement generation | `appinfo.json` (copyright/licence/links/acknowledgements) is read at **configure time** and generates C++ initialisation; the third-party list is generated from the dependency table | a second application target appears, or a release package needs the same metadata | `../design/appfw-app-info.md` F6 |
