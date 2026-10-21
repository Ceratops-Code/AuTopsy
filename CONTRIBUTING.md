There are several ways to contribute to [AuTopsy](https://github.com/Ceratops-Code/AuTopsy).
Use this repository for AuTopsy bugs, questions, and contributions.

## Developing

AuTopsy is mostly coded in C++, with Python integration tests and CMake builds.

To develop AuTopsy:

* Follow [BUILDING.md](BUILDING.md), including the focused video and editing checks.
* Open a pull request against `main` in `Ceratops-Code/AuTopsy`.
* Keep existing copyright and license notices. Contributions use the applicable
  licenses described in [LICENSE.txt](LICENSE.txt); AuTopsy does not require the
  separate upstream contributor agreement.
* See [AGENTS.md](AGENTS.md) for the fork's integration boundaries.
* The [upstream development resources](https://audacity.gitbook.io/dev/) describe
  the inherited audio engine. They are useful references, not AuTopsy support channels.

### Developing Plug-ins

AuTopsy retains the audio engine's plug-in APIs, including Nyquist, LV2, VST2,
VST3, and Audio Units on macOS. Plug-in compatibility follows the inherited host;
the video extension is currently Windows-only.

## Testing / Finding bugs

Build from the relevant branch using [BUILDING.md](BUILDING.md). Report a problem
on its pull request, or [open an AuTopsy issue](https://github.com/Ceratops-Code/AuTopsy/issues/new/choose).
Include the build or commit, reproduction steps, and expected and actual results.
Use a small public sample when media is needed; keep private footage out of public reports.

Try to reduce the problem to a small project and identify whether it involves
picture, soundtrack audio, or the inherited audio tools.

Questions are welcome in [AuTopsy issues](https://github.com/Ceratops-Code/AuTopsy/issues).

## Translating

The inherited translations remain credited to their authors. Translation fixes
for this fork can be proposed through an AuTopsy pull request.

## Feedback & Feature requests

Use [AuTopsy's feature request form](https://github.com/Ceratops-Code/AuTopsy/issues/new/choose).
Describe the editing task and the result you need.

## Supporting Users

Help other users in [AuTopsy issues](https://github.com/Ceratops-Code/AuTopsy/issues).
The [upstream audio manual](https://manual.audacityteam.org/) covers inherited
audio tools; AuTopsy's video guide is in [README.md](README.md#video-tracks-windows-fork).

### Making video tutorials

Mention the AuTopsy version or commit and show which parts are picture and which
are soundtrack audio, so viewers can reproduce the editing workflow.
