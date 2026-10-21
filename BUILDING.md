# Building AuTopsy

## Prerequisites

* **python3** >= 3.8 (you can find an official download here: <https://www.python.org/downloads/>)
* **cmake** >= 3.16 (you can find an official download here: <https://cmake.org/download/>)
* A working C++ 17 compiler
* Graphviz (optional)

For Windows see below for important installer settings.

Please note that Xcode 14 support on macOS requires CMake 3.24.0 or later.

### Conan

AuTopsy uses [Conan 2](https://conan.io/) to manage dependencies. If `conan` is not already installed, AuTopsy will download and install it automatically.

However, if you want to install Conan manually, you can do so by following the instructions on the [Conan website](https://docs.conan.io/en/latest/installation.html).
Manual installation can be useful if you want to use Conan to manage dependencies for other projects or if you plan to have multiple builds of AuTopsy on the
same machine.

### CMake

On Windows, please use the [prebuilt binaries](https://cmake.org/download/). Ensure you select one of the options to add cmake to the system path.

On macOS, the easiest way to install CMake is `brew install cmake`.

On Linux, `cmake` is usually available from the system package manager. Alternatively, `sudo pip3 install cmake` can be used to install the latest version of CMake.

### Windows

We build AuTopsy using [Microsoft Visual Studio](https://visualstudio.microsoft.com/vs/community/) 2022. In order to build AuTopsy **Desktop development with C++** workload is required.

### macOS

We build AuTopsy using XCode versions 12 and later. However, it is likely possible to build it with XCode 7.

### Linux

We use GCC 9 and later, but any C++17 compliant compiler should work.

Here are the dependencies you need to install on various distribution families.

#### Debian and Ubuntu

```text
$ sudo apt-get update
$ sudo apt-get install -y build-essential cmake git python3-pip
$ sudo pipx install conan
$ sudo apt-get install libgtk2.0-dev libasound2-dev libjack-jackd2-dev uuid-dev
```

#### openSUSE

```text
$ sudo zypper refresh
$ sudo zypper install patterns-devel-C-C++-devel_C_C++ cmake git python3-pip \
                      gtk2-devel libjack-devel uuid-devel libSM-devel
$ sudo pip3 install conan
```

#### Fedora Workstation

```text
$ sudo dnf update
$ sudo dnf install gcc-c++ cmake git python3-pip perl-core \
                   gtk2-devel gtk3-devel alsa-lib-devel jack-audio-connection-kit-devel uuid-devel libSM-devel
$ sudo pip3 install conan
```

### Graphviz

<https://graphviz.org/download/>

This is not necessary to build and run AuTopsy. It generates diagrams that aid understanding of the large scale structure of AuTopsy's source code.

If you install Graphviz, then an image file modules.dot.svg is generated in the build directory as a by-product of configuration. It shows the dependencies among the AuTopsy executable, its optional extension modules, its shared libraries, and third-party libraries.

You will also be able to change to the scripts directory and run ./graph.pl to generate a diagram of dependencies among source code files within the executable.

Native CTest clients use the persistent base Python runtime, rather than a
temporary uv dependency environment. An explicit test session refreshes that
CTest configuration without rebuilding or changing the accepted installer.

## Building on Windows

1. Ensure the Python installer option `Add Python 3.x to PATH` is checked. Go to Windows Settings "Add or Remove Programs" and check the `Add Python to environment variables` in Python settings if Python is not in `PATH`.
  
2. Use this AuTopsy source checkout, which includes the video extension
   and is based on the upstream audacity3 branch.

3. Open CMake GUI.

   Set the **Where is the source code** to the location of this AuTopsy checkout.

   Set **Where to build the binaries** to the location you want to place your build in. It is preferred that this location is not within the directory with the source code.

4. Press **Configure**. You can choose which version of Visual Studio to use and the platform to build for in the pop-up. We support **x64** and **Win32** platforms. The **x64** platform is a default option. Press **Finish** to start the configuration process.

5. After successful configuration, you will see `Configuring done` in the last line of the log. Press **Generate** to generate the Visual Studio project.

6. After you see "Generating done", press **Open Project** to open the project in Visual Studio.

7. Select "Build -> Build Solution".

8. You can now run and debug AuTopsy!

Generally, steps 1-6 are only needed the first-time you configure. Then, after you've generated the solution, you can open it in Visual Studio next time. If the project configuration has changed, the IDE will invoke CMake internally.

### Building with ASIO support on Windows

To enable ASIO support, please select `audacity_has_asio_support=On` in CMake after the initial configuration and then run select **Configure** again as described above. ASIO is only supported on Windows and only for 64-bit builds.

## macOS

1. Clone AuTopsy from its GitHub project.
  
    ```text
    $ git clone https://github.com/Ceratops-Code/AuTopsy/
    ```

2. Configure AuTopsy using CMake:

   ```text
   $ mkdir build && cd build
   $ cmake -GXcode ../AuTopsy
   ```

3. Open AuTopsy XCode project:

   ```text
   $ open Audacity.xcodeproj
   ```

   and build AuTopsy using the IDE.

Steps 1 and 2 are only required for first-time builds.

Alternatively, you can use **CLion**. If you chose to do so, open the directory where you have cloned AuTopsy using CLion and you are good to go.

In upstream releases before version 3.2 only **x86_64** builds and XCode "legacy" build system were supported. In order to build older version please use:

```text
   $ mkdir build && cd build
   $ cmake -GXcode -T buildsystem=1 ../AuTopsy
```

to configure AuTopsy.

## Linux & Other OS

1. Clone AuTopsy from its GitHub project.
  
    ```text
    $ git clone https://github.com/Ceratops-Code/AuTopsy/
    ```

2. Configure AuTopsy using CMake:

   ```text
   $ mkdir build && cd build
   $ cmake -G "Unix Makefiles" ../AuTopsy
   ```

   By default, Debug build will be configured. To change that, pass `-DCMAKE_BUILD_TYPE=Release` to CMake.

3. Build AuTopsy:

   ```text
   $ make -j`nproc`
   ```

4. Testing the build:
   Adding a "Portable Settings" folder allows AuTopsy to ignore the settings of any existing AuTopsy installation.

   ```text
   $ cd Debug/bin
   $ mkdir "Portable Settings"
   $ ./audacity
   ```

5. Installing AuTopsy

   ```text
   $ cd <build directory>
   $ sudo make install
   ```

## Advanced

### CMake options

You can use `cmake -LH` to get a list of the options available (or use CMake GUI or `ccmake`). The list will include documentation about each option.

### Building using system libraries

On Linux it is possible to build AuTopsy using (almost) only the libraries provided by the package manager. See the [required system libraries](linux/required_libraries.md).

```text
$ mkdir build && cd build
$ cmake -G "Unix Makefiles" \
        -Daudacity_use_ffmpeg=loaded \
        -Daudacity_lib_preference=system \
        -Daudacity_obey_system_dependencies=On \
         ../AuTopsy
```

There are a few cases when the local library build is preferred:

1. **wxWidgets**: While AuTopsy on **Linux** uses vanilla version of wxWidgets, we **require** that version **3.1.3** is used. This version is not available in most of the distributions.
2. **portaudio-v19**: AuTopsy currently uses [some private APIs](https://github.com/audacity/audacity/issues/871), so using system portaudio is not yet possible.
3. **vamp-host-sdk**: Development packages are not available in Ubuntu 20.04.
4. **libnyquist** & **portmixer**: Libraries are not available in Ubuntu 20.04.
5. **sqlite3** & **libsmbs**: Libraries are very outdated in Ubuntu 20.04.

It is not advised to mix system and local libraries, except for the list above. `ZLib` is a very common dependency; it is possible to mix system and local libraries in one build. However, we advise against doing so.

The [Linux package definitions](linux/packages) contain container environments
and dependency setup for the supported distribution packages.

System package discovery uses `pkg-config`. The distribution-specific setup
scripts in [linux/packages](linux/packages) describe their package dependencies.

### Disabling Conan

Conan can be disabled completely using `-Daudacity_conan_enabled=Off` during the configuration.
This option implies `-Daudacity_obey_system_dependencies=On` and disables `local` for packages that are managed with Conan.

### Disabling pre-built binaries downloads for Conan

It is possible to force Conan to build all the dependencies from the source code without using the pre-built binaries. To do so, please pass `-Daudacity_conan_allow_prebuilt_binaries=Off` to CMake during the configuration.

Additionally, passing `-Daudacity_conan_force_build_dependencies=On` will force Conan to rebuild all the packaged during *every* configuration. This can be usefull for the offline builds against the Conan download cache.

### Custom Conan profiles

AuTopsy will try to detect the host and build profiles automatically. However, `audacity_conan_host_profile` and `audacity_conan_build_profile` CMakes options can be used to specify custom Conan profiles.

### Troubleshooting Conan issues

To fix errors, similar to the following:

```text
ERROR: HTTPSConnectionPool(host='center.conan.io', port=443): Max retries exceeded with url: /v1/ping (Caused by SSLError(SSLCertVerificationError(1, '[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: certificate has expired (_ssl.c:1131)
```

please, update your Conan version. Alternatively, the problem can be fixed using:

```text
$ conan config install https://github.com/conan-io/conanclientcert.git
```

For errors like:

```text
Unable to connect to conan-center=https://conan.bintray.com
```

please run

```text
conan remote remove conan-center
```

For errors like:

```text
[package] package_name/package_version: package has 'exports_sources' but sources not found
```

please run

```text
conan remove package_name -c
```

### Reducing Conan cache size

In order to reduce the space used by Conan cache, please run:

```text
$ conan cache clean "*"
```

### Selecting target architecture on macOS

Starting with version 3.2.0, AuTopsy target architecture on macOS can be selected by passing `MACOS_ARCHITECTURE` to the CMake during the configuration.

To build for Intel:

```text
$ cmake -GXcode -DMACOS_ARCHITECTURE=x86_64 ../AuTopsy
```

To build for AppleSilicon:

```text
$ cmake -GXcode -DMACOS_ARCHITECTURE=arm64 ../AuTopsy
```

The default build architecture is selected based on `CMAKE_HOST_SYSTEM_PROCESSOR` value.

When cross-compiling from Intel to AppleSilicon, or if *Rosetta 2* is not installed on the AppleSilicon Mac,
a native host-architecture build directory is required, as AuTopsy needs a working `image-compiler`.

For example, to build ARM64 version of AuTopsy on Intel Mac:

```text
$ mkdir build.x64
$ cmake -GXcode -DMACOS_ARCHITECTURE=x86_64 -B build.x64 -S ../AuTopsy
$ cmake --build build.x64 --config Release --target image-compiler
$ mkdir build.arm64
$ cmake -GXcode -DMACOS_ARCHITECTURE=arm64 -DIMAGE_COMPILER_EXECUTABLE=build.x64/utils/RelWithDebInfo/image-compiler -B build.arm64 -S ../AuTopsy
$ cmake --build build.arm64 --config Release
```

This will place ARM64 version into `build.arm64/Release/`.

### Building with VST3SDK without Conan (Linux only)

Set one of the following environment variables to the path to the VST3 SDK (i.e. the folder containing the `pluginterfaces` folder):

* `VST3_SDK_DIR`
* `VST3SDK_PATH`
* `VST3SDK`

or copy the VST3 SDK to `vst3sdk` directory in the AuTopsy source tree.

Pass `-Daudacity_use_vst3sdk=system` to CMake. CMake will build the SDK during the configuration.

## Video tracks (Windows fork)

The video module is opt-in, so the base build does not require video SDKs.
The configuration below enables the AuTopsy video features explicitly.

Use the C++ workload and Windows SDK from Visual Studio 2022, and an extracted
64-bit libmpv development package matching the build architecture. The package
must contain include/mpv/client.h and libmpv-2.dll at its top level.
Windows package links are listed on [mpv's installation page](https://mpv.io/installation/).
Also provide the Windows x64 FFmpeg 8.1 GPL shared runtime, including libx264,
libx265 and libsvtav1 encoders, available from
[BtbN's FFmpeg builds](https://github.com/BtbN/FFmpeg-Builds/releases).
Keep ffmpeg.exe, ffprobe.exe, avformat-62.dll, avcodec-62.dll, avutil-60.dll,
swresample-6.dll, swscale-9.dll, avfilter-11.dll, avdevice-62.dll, and the
distribution's LICENSE.txt together in the selected runtime directory.
The command-line encoder and picture probe ship beside AuTopsy and run without a
console window. libmpv requires the C numeric runtime locale; Windows language
setup keeps that locale stable after capturing the regional display separator.

Configure the normal AuTopsy build with these additional options:

    cmake -S . -B _build -G "Visual Studio 17 2022" -A x64 -Daudacity_has_tests=ON -Daudacity_has_video_preview=ON -Daudacity_video_mpv_sdk="<extracted-mpv-sdk>" -Daudacity_video_ffmpeg_runtime="<ffmpeg-runtime>"
    cmake --build _build --config Release --parallel 4
    ctest --test-dir _build -C Release -R "^(video-playback|video-timeline|fork-groups|fork-tracks|fork-video|fork-media|fork-preview)$" --output-on-failure
    cmake -S . -B _build -DINNO_SETUP_COMPILER="<Inno-Setup-6.7.3>/ISCC.exe"
    cmake --build _build --config Release --target innosetup

The executable is AuTopsy.exe; internal CMake target names remain upstream's.
The published product branch is `main`; use `--branch main` when cloning.
Build the default target before running the app integration cases: it includes
the runtime modules, including modules that link to the application executable.
Building only the main executable after renaming it leaves those modules stale.
The distributable is `_build/package/autopsy-win-<version>-<content-id>-x64.exe`.
Install that installer into `%ProgramFiles%\AuTopsy`; do not copy a build tree
or run `cmake --install` against the installed application. The packaging
target uses that command only inside its disposable private staging directory.
The installer is also the asset attached to GitHub Releases. Source archives
are provided by GitHub for GPL access, not used for local deployment.
`BuildInnoSetupInstaller.cmake` owns `_build/innosetup`, recreates it for each
package, stops on failed staging, and removes it after success or failure.
It publishes completed installers atomically in `_build/package`, with a content
ID in each filename, and retains the current installer and at most two older
ones per architecture. Its private pending copy is removed at the next build.
A process lock prevents simultaneous packaging from removing live staging files.
Set audacity_has_video_preview=OFF to build without the video feature or SDKs.
The inherited binary updater is excluded from AuTopsy builds; its service offers
upstream releases rather than AuTopsy builds. Product help and feedback link to
this fork's repository. Upstream source updates use the manual process in README.md.
The preview code is currently Windows-only; other platforms keep their existing
build behavior. The transport CTest case covers transport ownership, repeated
calls, interruptions before and after each player command, asynchronous seeks,
loop boundaries, frame quantization, and invalid clock values.
The timeline case exercises the owned value model without a host editor.
The fork cases use public scripting commands against synthetic media in a
marked temporary work directory. Python 3 and Pillow (for the owned-window screenshot)
are needed for those cases. Cases check native video/audio track ownership,
cross-track clip dragging, clipboard edits, project round trips, group motion,
track order and scrolling, mixed-source rendering, video-only playback,
silence, rejected edits and overwrite protection.
Preview tests always generate a neutral 16-second 1080p colour pattern and tone;
they accept no personal media path or environment override. The generated
`_fork-tests/_preview/source.mp4` belongs to the marked test root and is replaced
when that case starts afresh. The fixture producer atomically publishes only
completed footage and discards its private pending file after interruption.
`fork-fixtures` checks the encoded picture/audio and repeated/interrupted calls
without opening AuTopsy; its temporary media is removed after the check.
The gesture case waits for the native pane to be enabled, then supplies held
mouse input only to the owned test window through its normal window procedure.
The GUI runner selects Windows DISPLAY2 when connected. With several displays
it excludes DISPLAY1; with one it uses that display. It seeds only its private
window preferences and requires the scripting module's test display guard before
launching. That process-local adapter constrains each wx top-level window before
Windows applies its placement, covering the loading splash, editor, welcome,
About, and video preview. A missing or too-small target stops the test instead
of falling back to screen 1. It changes no system display settings or user windows.
Each placement resolves the selected device afresh in the callback's DPI
coordinate space. The guard retains neither monitor handles nor geometry as an
identity, and never falls back to screen 1. Private test windows use wx's
stay-on-top style so the host retains their exposure during activation; the
runner also checks the window under the target point before sending input.
Its held mouse button is released even when the owned window disappears.
`fork-display-policy` checks device selection without UI; `fork-display-rejection`
checks that a disconnected target stops the child before showing a window.
`fork-branding` also repeats hidden/visible placement requests toward the other
screen, including oversized windows. GUI cases check the owned windows before
commands, mouse input, and screenshots; pre-show observations stay in each case's
bounded scratch output and its result records name the selected display.
Unreadable display information stops the test before placement; the app log
keeps the monitor query's failure details. `fork-worker-lifecycle` checks that
app failure stops its worker promptly, preserves both streams, and releases
the owned child. Its temporary fixtures are removed after each check.
The runner owns its marked case directories and shared preview cache under
the selected test work root. Fresh runs replace case scratch data. Completed
result records retain their original run, executable and check identities in
_accepted, with at most three records per case/input and 36 records overall;
playback observations are included. Receipts do not resume or skip test work.
Portable Settings is removed when the owned app exits.

The SDK remains at the caller-selected path and is read during the build.
CMake owns the single current libmpv-2.dll, the seven named FFmpeg DLLs,
ffmpeg.exe, ffprobe.exe and
FFmpeg-LICENSE.txt beside the built executable. It
copies these files on every app build, restoring missing files and replacing
them when building with another SDK or compatible FFmpeg package. Build outputs belong to the chosen
build directory; no version copies or recovery checkpoints are created by this
feature. The decoder reads the selected video without writing player
configuration, logs or cache files. Native video tracks own their per-clip source
references and timing in the existing .aup3 XML storage. No media copy or separate
database is introduced. Soundtrack audio uses the existing project audio storage,
Undo history, and cleanup. Closing the project releases its player and window.

For isolated test preferences, use the existing portable-settings mode: the
literal directory name Portable Settings is required beside the executable.
AuTopsy owns that profile and its configured temporary audio directory.
Installation excludes this test profile and test executables. Installed
preferences use autopsy.cfg under the user's AuTopsy data directory;
temporary project audio uses its SessionData directory. AuTopsy's profile is
separate.

Keep the libmpv package's applicable license and corresponding-source material
with any distributed build, alongside AuTopsy's existing license material.

### Windows installer workflow

Run the same repository-owned builder used by the GitHub-hosted Windows job:

    uv run --project scripts --locked --with cmake==3.31.10 python scripts/build/windows/build_video_installer.py --build-dir .build/_windows-installer

Install Visual Studio 2022's C++ workload and Inno Setup 6 first. Use
`--inno-compiler` for a nondefault compiler location. The builder uses CMake's
archive reader, so a separate 7-Zip installation is unnecessary. Python build
dependencies, including the Windows process-tree wrapper, come from the locked
`scripts` environment.

The builder owns `_video-dependencies` under its selected build directory:
one pinned archive and current SDK per component, private extraction/download
siblings, and one identity record per SDK. Startup discards only abandoned
owned staging; successful publication removes staging immediately. Required
SDK filenames are kept because native linkers and encoders require them.
A live-worker lock protects cleanup. A Windows job terminates remaining
extractor/compiler children when their worker exits, including abrupt exits.

The packager publishes `package/_installer-x64.json` atomically, naming the
exact installer and its SHA-256. The builder consumes that descriptor and writes
one current `package/_build-manifest.json` with the source revision, unsigned
status, installer hash and pinned dependency identities. Failure replaces
`_build_command_failure.json` with the exact invocation, exit and both output
streams; success removes it. Pending record files are removed on the next call.
The existing per-architecture installer retention remains current plus two
predecessors. Building never installs or publishes an application.

GitHub's `Validate` workflow builds on `windows-2022` when manually
dispatched, or when a pull request has the `build-windows-installer` label.
Its repository checks precede the installer job. After building, it runs the
same focused native project-compatibility case used locally, against pinned
portable Audacity consumers. The uploaded unsigned installer and source manifest
are retained for 14 days; failure details for 7 days.
Signing is a separate service approval and is not claimed by this job.

The `deliverables.packages.windows-installer.actions.publish` operation consumes
the existing accepted installer and `_build-manifest.json` from
`AUTOPSY_VIDEO_BUILD_DIR`. Set `AUTOPSY_RELEASE_TAG` and `AUTOPSY_RELEASE_NOTES`
to select the preview tag and a local UTF-8 notes file. The tag names the original
build revision, including when later test/documentation commits have advanced
main. The operation creates a draft only when needed, uploads just those two
accepted files, checks GitHub's asset digests, then makes the preview public.
Repeated calls inspect live release state and perform only missing effects;
conflicting tags/assets stop publication. It preserves the original build
acceptance and does not compile or install an application.

Every new installer build runs `fork-fixtures` after compilation and before
packaging, without opening AuTopsy. The build manifest records its passing result
and source revision. Repeated builds reuse that accepted installer only when the
clean source, dependency inputs and installer integrity still match the receipt.
Portable model and installer-producer tests run in the repository test operation;
source-only CI does not require an already-built Windows application.
The package build operation takes explicit `build_dir` and `inno_compiler`
inputs, so local builds can reuse their task-owned build and compiler paths.
Setting both `AUTOPSY_VIDEO_BUILD_DIR` and `AUTOPSY_VIDEO_TEST_NAME` selects only
an explicit native session in the repository test operation. For example,
`^fork-media$` runs the existing guarded desktop case for linked-media splits
across video tracks, trims with unrelated audio before zero, saved links, flags
and exports. Reconfiguration without the selector restores the portable suite.
`^fork-project-compatibility$` automatically prepares the baseline Audacity 3.7.8
and the selected update consumer 3.7.9 from official Windows x64 portable ZIPs.
`prepare_audacity_consumer.py` owns their release URLs and SHA-256 pins, taken from
the official [3.7.8 assets](https://github.com/audacity/audacity/releases/expanded_assets/Audacity-3.7.8)
and [3.7.9 assets](https://github.com/audacity/audacity/releases/expanded_assets/Audacity-3.7.9).
Changing a supported consumer means updating those pins and running this case;
upstream updates must keep a baseline while exercising the new consumer.
No Audacity installation, extracted reader module, installed profile or user
project is an input. This remains a test dependency, outside AuTopsy's installer.

The cache owner groups bundles under the native build's
`_fork-tests/_audacity-consumers`. One OS lease covers preparation and use, so a
live consumer cannot lose its package to another call. Entry removes abandoned
staging and superseded owned bundles; success keeps only the two declared
consumers, with accepted payload hashes for integrity reuse. Checksum, download,
archive path, link, size or extraction failures publish no partial package.
Archives are removed after extraction; staging is removed after failure, or at
the next entry after a killed worker. The owner keeps one marker, one lock file
and one receipt per active package, with no diagnostic history. Unrelated paths
are left alone. `fork-audacity-consumer-cache` covers repeat, interruption,
concurrent-owner, corruption, archive safety and cleanup behavior without network
access or an application window.

Each native consumer runs from a private copy in the marked case directory.
Its private portable profile disables file association and update changes,
separates audio and GUI temporary storage, and places its project window on the
selected test display. The upstream name `Portable Settings` is required for
that private profile; cache and test-owned names otherwise use `_` prefixes.

This case opens a generated video/audio/label project and a video-only project,
twice each, then checks stock save/copy/reopen behavior. It compares decoded
48-kHz stereo audio with a maximum sample difference of 0.0005 to allow export
dither, while clip timing, names, track properties and labels must match. Result
records name both consumer versions, official package digests and executable hashes. After all saved-copy
acknowledgements the harness ends only its private consumer process; stock pipe
shutdown is outside this file-format test. It removes the private copied app,
profile, log and working copies on success or failure. Current neutral fixtures
and original test projects remain in the existing bounded per-case workspace.

For an explicit separate media-check session, `AUTOPSY_VIDEO_BUILD_DIR` selects
an existing native build, defaulting to
`.build/_windows-installer`. For a separate desktop test session,
`AUTOPSY_VIDEO_TEST_NAME` selects a CTest case such as `^fork-media$`; empty
selections fail. The same builder's `--test-existing-build`, `--build-dir` and
`--test-name` options expose those inputs directly. Test child failures own one
current `_test_command_failure.json` beside the build; success removes it and
startup discards its abandoned pending sibling.
The application's install operation takes `installer_path` from the accepted build
manifest and runs that installer silently. It installs into Program Files/AuTopsy;
close the application first. Deployment never copies a source/build tree.
The media case covers linked video/audio movement and trim/split behavior,
saved clip flags, relinking, source frame stepping and modern software exports.

### Independent feature checks

The SDLC `repository.actions.test` operation and GitHub CI call the same owning
runner. Its direct developer entry point is:

    uv run --project scripts --locked --with cmake==3.31.10 python scripts/run_affected_tests.py

The runner compares against the merge base with `origin/main`, including current
staged/unstaged and untracked inputs. Set `AUTOPSY_TEST_BASE` or use `--base REF`
for another comparison. CI supplies the event's base revision; an unavailable
shallow base conservatively selects all portable checks. `--all` explicitly
selects that full scope. Docs-only changes need no portable tests.

One input map in `run_affected_tests.py` selects dependent groups; unknown owned
executable inputs require a map entry or an explicit scope decision. CMake builds
only their targets, and Catch2/CTest discovers individual compiled case names.
Separate suites cover edit plans, composition geometry, existing playback,
branding, installer production, harness policy/lifecycle and affected selection.
Native adapter inputs are reported separately, without implying desktop acceptance.

The runner owns `.build/_core` and `.build/_test-results`. Immutable results retain
their original IDs and source/test/fixture/lock/executable/runtime/command inputs.
Matching passing results can be reused unless a newer applicable failure exists;
source changes during compilation or execution cannot earn a pass. At startup and
after publication, receipts retain the current and at most two earlier records per
named check. An OS lock protects active work; its one lock file persists, while
pending files are removed under exclusive ownership. Other files are untouched.
The standard-library test interpreter is bound to persistent `sys.base_prefix`,
so a CMake cache never depends on a discarded temporary `uv --with` environment.
Later test changes do not rebuild or revoke an unchanged accepted installer.

This mode checks owned video timing, transport, edit plans, crop/PiP geometry and product-name policy
without the editor or media SDKs. It fetches the same Catch2 2.13.8 revision used by the upstream
test interface, pinned to its Git commit. CMake owns the current executables,
dependency checkout, and generated files in the selected build directory.
The full Windows cases above cover the native adapters, media, and GUI; the
independent CI checks do not claim those desktop behaviors were exercised.
The product-name policy lives in libraries/lib-strings/ApplyProductBranding.h; its small
adapter uses the existing fork-name translation hook and returns owned values.
Translated templates receive the product name before formatting arguments.
Original gettext keys, contexts, plural rules, debug identities, literal credits,
and argument values keep their own meaning. Technical paths, links, identifiers,
and markup attributes retain their spelling. The core case has no wxWidgets
dependency; the native product-branding case also uses a real in-memory gettext
catalog to cover translation, formatting, and the adapter contracts.
`fork-branding` additionally checks the welcome screen and native Help menu,
including opening and closing About twice. It uses only its private test process.

Inherited Audacity 4 and upstream release-publishing jobs are restricted to the
official repository. This fork's workflow checks its own feature models and
does not fetch or merge upstream updates automatically.
