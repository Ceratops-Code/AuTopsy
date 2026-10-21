# Building AuTopsy

## Prerequisites

* **python3** >= 3.8 (you can find an official download here: https://www.python.org/downloads/)
* **cmake** >= 3.16 (you can find an official download here: https://cmake.org/download/)
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

```
$ sudo apt-get update
$ sudo apt-get install -y build-essential cmake git python3-pip
$ sudo pipx install conan
$ sudo apt-get install libgtk2.0-dev libasound2-dev libjack-jackd2-dev uuid-dev
```

#### openSUSE

```
$ sudo zypper refresh
$ sudo zypper install patterns-devel-C-C++-devel_C_C++ cmake git python3-pip \
                      gtk2-devel libjack-devel uuid-devel libSM-devel
$ sudo pip3 install conan
```

#### Fedora Workstation

```
$ sudo dnf update
$ sudo dnf install gcc-c++ cmake git python3-pip perl-core \
                   gtk2-devel gtk3-devel alsa-lib-devel jack-audio-connection-kit-devel uuid-devel libSM-devel
$ sudo pip3 install conan
```

### Graphviz

https://graphviz.org/download/

This is not necessary to build and run AuTopsy. It generates diagrams that aid understanding of the large scale structure of AuTopsy's source code.

If you install Graphviz, then an image file modules.dot.svg is generated in the build directory as a by-product of configuration. It shows the dependencies among the AuTopsy executable, its optional extension modules, its shared libraries, and third-party libraries.

You will also be able to change to the scripts directory and run ./graph.pl to generate a diagram of dependencies among source code files within the executable.


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
  
    ```
    $ git clone https://github.com/Ceratops-Code/AuTopsy/
    ```

2. Configure AuTopsy using CMake:
   ```
   $ mkdir build && cd build
   $ cmake -GXcode ../AuTopsy
   ```

3. Open AuTopsy XCode project:
   ```
   $ open Audacity.xcodeproj
   ```
   and build AuTopsy using the IDE.

Steps 1 and 2 are only required for first-time builds. 

Alternatively, you can use **CLion**. If you chose to do so, open the directory where you have cloned AuTopsy using CLion and you are good to go.

In upstream releases before version 3.2 only **x86_64** builds and XCode "legacy" build system were supported. In order to build older version please use:

```
   $ mkdir build && cd build
   $ cmake -GXcode -T buildsystem=1 ../AuTopsy
```

to configure AuTopsy.

## Linux & Other OS

1. Clone AuTopsy from its GitHub project.
  
    ```
    $ git clone https://github.com/Ceratops-Code/AuTopsy/
    ```

2. Configure AuTopsy using CMake:
   ```
   $ mkdir build && cd build
   $ cmake -G "Unix Makefiles" ../AuTopsy
   ```
   By default, Debug build will be configured. To change that, pass `-DCMAKE_BUILD_TYPE=Release` to CMake.

3. Build AuTopsy:
   ```
   $ make -j`nproc`
   ```

4. Testing the build:
   Adding a "Portable Settings" folder allows AuTopsy to ignore the settings of any existing AuTopsy installation.
   ```
   $ cd Debug/bin
   $ mkdir "Portable Settings"
   $ ./audacity
   ```

5. Installing AuTopsy
   ```
   $ cd <build directory>
   $ sudo make install
   ```

## Advanced

### CMake options

You can use `cmake -LH` to get a list of the options available (or use CMake GUI or `ccmake`). The list will include documentation about each option.

### Building using system libraries

On Linux it is possible to build AuTopsy using (almost) only the libraries provided by the package manager. Please, see the list of required libraries [here](linux/required_libraries.md).

```
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

```
ERROR: HTTPSConnectionPool(host='center.conan.io', port=443): Max retries exceeded with url: /v1/ping (Caused by SSLError(SSLCertVerificationError(1, '[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: certificate has expired (_ssl.c:1131)
```

please, update your Conan version. Alternatively, the problem can be fixed using:

```
$ conan config install https://github.com/conan-io/conanclientcert.git
```

For errors like:

```
Unable to connect to conan-center=https://conan.bintray.com
```

please run

```
conan remote remove conan-center
```

For errors like:

```
[package] package_name/package_version: package has 'exports_sources' but sources not found
```

please run

```
conan remove package_name -c
```
### Reducing Conan cache size

In order to reduce the space used by Conan cache, please run:

```
$ conan cache clean "*"
```

### Selecting target architecture on macOS

Starting with version 3.2.0, AuTopsy target architecture on macOS can be selected by passing `MACOS_ARCHITECTURE` to the CMake during the configuration.

To build for Intel:

```
$ cmake -GXcode -DMACOS_ARCHITECTURE=x86_64 ../AuTopsy
```

To build for AppleSilicon:

```
$ cmake -GXcode -DMACOS_ARCHITECTURE=arm64 ../AuTopsy
```

The default build architecture is selected based on `CMAKE_HOST_SYSTEM_PROCESSOR` value.

When cross-compiling from Intel to AppleSilicon, or if *Rosetta 2* is not installed on the AppleSilicon Mac, 
a native host-architecture build directory is required, as AuTopsy needs a working `image-compiler`.

For example, to build ARM64 version of AuTopsy on Intel Mac:

```
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
Also provide the Windows x64 FFmpeg 8.1 shared runtime, available from
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
    ctest --test-dir _build -C Release -R "^(video-playback|video-timeline|fork-groups|fork-tracks|fork-video|fork-preview)$" --output-on-failure
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
The gesture case waits for the native pane to be enabled, then supplies held
mouse input only to the owned test window through its normal window procedure.
The GUI runner selects Windows DISPLAY2 when connected. With several displays
it excludes DISPLAY1; with one it uses that display. It seeds only its private
window preferences and requires the scripting module's test display guard before
launching. That process-local adapter constrains each wx top-level window before
Windows applies its placement, covering the loading splash, editor, welcome,
About, and video preview. A missing or too-small target stops the test instead
of falling back to screen 1. It changes no system display settings or user windows.
Placement and display-change checks obtain a fresh monitor handle from the last
checked target area and require the same device name. They never select the
nearest or primary monitor as a fallback, retain a monitor handle between
callbacks, or accept a removed/reassigned target. Desktop enumeration is needed
only for initial named-device discovery.
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

### Independent feature checks

The same CMake/CTest entry point is used by local contributors and GitHub CI:

    cmake -S src/video -B _core-build -DCMAKE_BUILD_TYPE=Release
    cmake --build _core-build --config Release --parallel 2
    ctest --test-dir _core-build -C Release --output-on-failure

This mode checks the owned video timing, transport, and product-name policy
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
