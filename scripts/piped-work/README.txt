=piped-work folder=

This folder contains scripts for exercising mod-script-pipe

test_fork_features.py dispatches scenarios and owns their immutable results.
run_native_test_sessions.py owns the marked private profile, process and native
case registry. With multiple screens it runs on
DISPLAY2 (or another screen except DISPLAY1); a single screen is allowed.
The rebuilt scripting module constrains test windows before showing them.
Missing display guards or an unavailable target stop the run. User windows and
system display settings are untouched. See BUILDING.md for CMake/CTest usage.

To run a simple test to check communications:
   python pipe_test.py
or:
   python3 pipe_test.py

A much longer test that produces many image.
This script requires files from the "tests/samples/" folder and writes images
to "/tests/results/" folder, both of which are in the root of the source tree.
   python docimages_all.py


AuTopsy native check results
---------------------------
test_fork_features.py owns _accepted under the selected test work root. The
existing native app/profile mutex excludes concurrent writers. Normal SDLC/CTest
calls plan each requested case from app and DLL bytes, affecting check modules,
media tools, Python/OS and display scope; matching completed passes keep their
original result identity, observations and check version without launching the
app. Newer matching failed/incomplete outcomes block an older pass for current
qualification. Historical records lacking scope are not upgraded or rewritten.

Attempt intents are atomic and observational. A fresh owner marks an interrupted
attempt incomplete instead of restoring execution steps. A result published
before interruption remains accepted; its leftover intent is removed. Startup
and completion prune temporary writes and keep three passes per case, the latest
matching blocking outcomes, and two other attempts (three with no pass).
Unrelated files remain untouched. This is the existing result archive, not a
second cache or a workflow checkpoint store.

scripts/test_native_test_results.py covers repeat/reuse, affecting inputs, failure
precedence, bounded cleanup and interruptions around intent, execution, result
publication and intent removal. Changes to orchestration select these checks
without automatically rerunning unrelated model tests or accepted app binaries.

Shared native sessions
----------------------

The SDLC affected-test runner batches compatible CTest commands using --cases.
The driver keeps one app process for explicitly registered single-project
scenarios. Each worker gets a new project: no tracks, clips, labels, Undo/Redo
history or changed shared preferences may carry into its next scenario. Menu
state is read without invoking Undo. A failed worker or reset retires that app;
remaining cases start afresh and completed passes retain their original IDs.
Startup, shutdown, capture/recovery, cold download and multi-project journeys
keep their fresh-process contract. Unknown cases never become shared implicitly.
Individual --case commands remain fresh-process checks. One existing Windows
mutex encloses profiles, generated media and results, and a kill-on-close job
contains the runner's children if the runner itself is stopped unexpectedly.

prepare_test_media.py owns _generated-media beneath the selected work root.
Exact recipes and tool bytes identify neutral media; scenarios get independent
copies. Complete caption weights are grouped by speech-worker bytes; partial
downloads and job files are excluded. Cold-download cases never seed models.
Startup and completed publication retain the current and two preceding groups
per purpose. Pending generation/copies are removed after interruption or at the
next normal entry; matching completed copies are left intact. Groups contain at
most sixteen files, 512 MiB per file and 1.5 GiB combined. Native fixture names
and model filenames retain their loader-required names inside these private
groups. _app.log retains a two-MiB tail; each fresh case replaces its old work.

native-test-sessions covers process reuse, isolation, failed reset/worker
retirement, interruption, bounded logs and cache publication/copy/cleanup.
For new runner checks of an unchanged release, SDLC also accepts
AUTOPSY_TEST_EXECUTABLE and AUTOPSY_VIDEO_TEST_NAME. The executable must be a
private extracted payload with _test_runtime_owner containing
"AuTopsy private test runtime 1". Results belong to .build/_native-runtime-tests;
this scope never builds or installs an application. The extraction caller owns
and removes that payload. Ordinary compiled-build checks continue using
AUTOPSY_VIDEO_BUILD_DIR and AUTOPSY_VIDEO_TEST_NAME.
