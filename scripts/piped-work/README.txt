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
