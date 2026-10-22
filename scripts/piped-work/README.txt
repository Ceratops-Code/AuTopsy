=piped-work folder=

This folder contains scripts for exercising mod-script-pipe

test_fork_features.py is the CTest runner for AuTopsy behavior cases. It owns a
marked private profile and child process. With multiple screens it runs on
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

scripts/test_run_affected_tests.py covers repeat/reuse, affecting inputs, failure
precedence, bounded cleanup and interruptions around intent, execution, result
publication and intent removal. Changes to orchestration select these checks
without automatically rerunning unrelated model tests or accepted app binaries.
