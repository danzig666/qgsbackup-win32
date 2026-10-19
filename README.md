# QGIS Project Backup

`qgsbackup.exe` is a native Windows x64 backup utility for QGIS `.qgs` and `.qgz` projects. It has a Unicode Win32 GUI and a backward-compatible command-line interface, both powered by the same backup engine. Python, QGIS, Qt, and the Visual C++ runtime are not required on the destination computer.

## Using the GUI

Double-click `qgsbackup.exe`, run it without arguments, or use `qgsbackup.exe --gui`. GUI launches detach immediately, so a terminal prompt is not held open until the window closes. Select a project and destination, choose whether to create a timestamped folder, and start the backup. The **Edit skip list...** button loads `qgsbackup_skip.txt` into an editor and saves it as UTF-8. The work runs on a background thread; progress, copied size against the total size to copy, warnings, cancellation, and an **Open backup folder** action are available in the window.

The GUI is shown in Hungarian when the Windows region (Settings → Time & language → Region → Country or region) is Hungary, and in English otherwise. The command line is always English.

The GUI remembers the last destination and the timestamp/skip-file choices under `HKCU\Software\QgsBackup`. Cancelling is cooperative. Partial copied data is deliberately retained for inspection; the original project and datasets are never modified.

## Command line

```cmd
qgsbackup.exe "C:\GIS\Ebes.qgz" "D:\Backups" --date
qgsbackup.exe "C:\GIS\Ebes.qgs" "D:\Backups" --verbose
qgsbackup.exe "C:\GIS\Ebes.qgz" "D:\Backups" --no-skip-file
qgsbackup.exe -ns "C:\GIS\Ebes.qgz" "D:\Backup folders\Ebes\\"
```

Syntax:

```text
qgsbackup [options] <project.qgs|project.qgz> <destination> [options]
qgsbackup --help
qgsbackup --version
```

CLI timestamping remains off by default for compatibility with the Python program. The GUI defaults it on as the safer interactive choice.
Backup options may be placed before, between, or after the project and destination. Quoted paths with spaces, Unicode characters, and a trailing backslash are accepted.

Exit codes:

| Code | Meaning |
|---:|---|
| 0 | Success, including success with warnings |
| 1 | General failure |
| 2 | Invalid arguments |
| 3 | Invalid or missing project |
| 4 | Destination error |
| 5 | Malformed XML/QGZ or output packaging error |
| 6 | Cancelled |

## Backup behavior

The destination contains the rewritten project, files beside the project that share its basename, and copied local resources under `layers/`. QGIS provider suffixes such as `|layername=roads` and `?type=csv` are preserved. Shapefile sidecars (`.dbf`, `.shx`, `.prj`, and others) are copied together. Directory datasources are copied recursively. Repeated sources are copied once, while same-named sources from different directories receive a deterministic eight-character MD5 path suffix.

Remote/database, memory, virtual, and embedded `base64:` sources are left unchanged. Missing local sources produce warnings and are also left unchanged. `.qgz` archives are extracted with ZIP traversal protection, modified, and rebuilt with DEFLATE compression while preserving all archive members.

`qgsbackup_skip.txt` is loaded from the executable directory. Patterns are case-insensitive and support `*` and `?`. A matching datasource is not copied; relative references are made absolute so they continue pointing to the original source instead of becoming broken inside the backup. A missing skip file is not an error.

## Build and test

Visual Studio 2019 Build Tools or newer with the x64 C++ workload is supported:

```cmd
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The statically linked release is written to `dist\qgsbackup.exe`, with the default skip file copied beside it. The application icon can be regenerated with:

```powershell
powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
```

TinyXML2 and miniz are vendored under `third_party`; their source files contain their upstream license notices.

## Layout

```text
src/          shared engine, XML/ZIP/path utilities, CLI, and Win32 GUI
third_party/  TinyXML2 and miniz
tests/        unit tests, integration fixture, and end-to-end test script
assets/       application icon
dist/         x64 release executable and skip file
```

Known limitation: QGIS supports many provider-specific datasource syntaxes. Recognized non-file providers are preserved, but an unusual plugin provider that resembles a local path may be reported as a missing file and left unchanged. QGIS itself is not needed to create a backup.
