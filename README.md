# AuTopsy


**AuTopsy** is a Windows fork of [Audacity](https://www.audacityteam.org), based on its audacity3 branch. It adds synchronized video preview, imports the video's soundtrack into ordinary editable audio tracks, and exports video with the edited audio mix. The upstream audio editor provides:

- **Recording** from any real or virtual audio device that is available to the host system.
- **Export / Import** a wide range of audio formats, extensible with FFmpeg.
- **High quality** using 32-bit float audio processing.
- **Plugin Support** for multiple audio plugin formats, including VST, LV2, and AU.
- **Macros** for chaining commands and batch processing.
- **Scripting** in Python, Perl, or any other language that supports named pipes.
- **Nyquist** a powerful built-in scripting language that may also be used to create plugins.
- **Editing** multi-track editing with sample accuracy and arbitrary sample rates.
- **Accessibility** for VI users.
- **Analysis and visualization** tools to analyze audio or other signal data.

## Users

Build AuTopsy using [BUILDING.md](BUILDING.md). A local installation runs from `%ProgramFiles%\AuTopsy\AuTopsy.exe`.
Upstream help for the audio editor is available [here](https://audacityteam.org/help/).

## Developers
Build instructions are available [here](BUILDING.md).

Additional development resources may be found [here](https://audacity.gitbook.io/dev/).

## License

Audacity is open source software licensed GPLv3. Most code files are GPLv2-or-later, with the notable exceptions being /lib-src (which contains third party libraries), as well as VST3-related code. Documentation is licensed CC-by 3.0 unless otherwise noted. Details can be found in the [license file](LICENSE.txt).

## Video tracks (Windows fork)

This fork follows upstream's audacity3 branch and adds native video tracks in
the same scrolling track pane as audio. Each track can hold clips from several
files. A separate, resizable preview window shows the composed picture; higher
visible tracks take precedence and gaps expose the tracks below.

AuTopsy has its own Windows instance lock, settings directory, and scripting
pipes, so it can run alongside an installed copy of Audacity.

1. Choose **File > Import > Video...**, the first import entry. Each import
   adds a video track; **Tracks > Add New > Video Track** adds an empty one.
2. Its soundtrack is imported into new audio tracks at its original timing,
   without music tempo matching. Edit, mute, or delete those tracks normally;
   the video remains editable. Silent videos add no audio tracks.
3. Use the transport controls. Video follows normal-speed playback, pause,
   stop, timeline seeks, and loop jumps. The preview player itself is muted,
   so audio plays once through the editable project tracks.
4. Closing the preview hides it. **View > Video Preview** shows it again, and
   starting playback also brings it back. Delete a video track using its normal
   close button or **Tracks > Remove Tracks**.
5. Choose **File > Export Video with Audio Mix...** to save MP4 or MKV.
   Unchanged picture timing is copied without re-encoding, including a source
   split into adjacent clips. Moving or deleting picture sections renders the
    edited picture as MPEG-4 Part 2 at the first visible source's frame rate and
    oriented picture size. Other sources are scaled and letterboxed. The entire project mix replaces
   the original soundtrack, including mute, solo, gain, pan and effects.
   Export starts at zero and ends at the last video clip; short audio is padded
   with silence and later audio is excluded. The current selection does not
   limit this export. Audio is stereo AAC at 48 kHz, 256 kbit/s.

Playback uses the normal audio transport, including when the soundtrack has
been deleted. Video-only projects supply silence in memory to its clock; no
hidden audio track or temporary soundtrack is created.

Drag a clip's header along the timeline or onto another video track with free
space. Drag either edge to trim it; click its picture body to select time.
Use the normal **Edit > Clips > Split**, **Split New**, **Trim**, Cut, Copy,
Paste, Delete, and Duplicate actions. Right-click a clip for its editing menu.
Splits and moves snap to source frames. Picture edits leave audio available
for independent synchronization and mixing. Tracks can be resized, reordered,
scrolled, selected, renamed, and removed with the native controls.

Video tracks own their source references and clip timing. Native TrackList,
clipboard, selection and history adapters provide normal Undo/Redo. Track and
clip data are saved in .aup3 XML and restored without importing another copy
of the soundtrack. Projects from the earlier single-video format open as a
native video track. Source files remain external and must stay accessible.
Cancelling or failing an import keeps existing tracks. Importing picture with
audio is one combined Undo action; removing its picture leaves the audio.

Synchronization uses the audio clock with periodic drift correction;
the preview has no guarantee of frame-accurate external
monitoring. Play-at-speed, scrub, and time-track warping are outside the first
version's smooth-playback scope.

The decoder is [libmpv](https://mpv.io/), loaded from this build's own folder.
Hardware decoding is enabled. Sources above 720p or 60 fps get a silent 540p,
30 fps preview copy during import; edited-audio exports still use the original.
`PrepareVideoPreview.cpp` owns `_video-previews` in the active settings folder.
It groups entries by source path and file identity, retains at most the current
version and two predecessors per source (36 entries total), and protects live
entries with leases. Component startup and completed preparation remove orphaned
builds and prune old entries. Only completed preview/thumbnail pairs are reused.
Reopening uses an available cache without a nested loading dialog.
`GetVideoPreviewInfo` reports decoded size and a bounded observation of the last
playback, including dropped frames and video/audio clock positions.
Soundtrack import uses Audacity's existing FFmpeg importer with bundled FFmpeg
8.1 shared libraries. Video export uses bundled `ffmpeg.exe` and `ffprobe.exe`; if a source
video codec cannot be copied into MP4, export MKV instead. Export cannot replace
the source video. A cancelled or failed export preserves an existing destination.

The export component owns one temporary `_autopsy-video-export-*` directory
beside the destination, containing `_owner`, `_mix.wav`, optional rendering
instructions in `_filters.txt`, and `_result.mp4` or `_result.mkv`.
`/VideoExport/Workspace` in the active settings profile records
only that directory. Project startup and each export remove abandoned work;
an exclusive owner-file lock protects a live export. Success and cancellation
remove temporary files. Publishing uses one rename on the destination volume.
Each chosen output path holds one current export; completed files belong to
the user and are replaced only when overwrite is allowed. No export log or
version history is produced by the application.

Windows scripting clients use the names
`AuTopsyToSrvPipe` and `AuTopsyFromSrvPipe`.
See [BUILDING.md](BUILDING.md#video-tracks-windows-fork) for the SDK
and focused test commands.

The `ImportVideo: Filename="..."` and
`ExportVideoMix: Filename="..." Overwrite=0` scripting commands invoke the
same operations as the File menu. Set `Overwrite=1` to replace an existing
export; the source video is always protected.

`GetVideoClips` returns native track indices and each clip's source, ID and range.
`SplitVideo: At=2`, `MoveVideoClip: Track=0 Id=2 ToTrack=1 Start=5`, and
`DeleteVideoClip: Track=0 Id=2` use the same editing operations as the native
video controls. Omit `ToTrack` for a move within the source track. Omit `Track`
only when focus or an unambiguous clip ID identifies the track. Empty tracks
can be added through `NewVideoTrack`, `AddVideoTrackBefore: Track=1`, and
`AddVideoTrackAfter: Track=1`. Repeating a split at an existing boundary,
an absolute move to the same start, or deletion of an absent ID makes no change.

## Audio clip groups and track insertion

The fork adds **Edit > Clips > Group Clips...** and
**Ungroup Clips...**. The chooser can select individual clips across tracks.
Grouping merges any existing groups touched by the choice. A normal clip-header
drag then moves the whole group, including members on unselected tracks, subject
to every track's available space. Group IDs survive project saves and Undo/Redo.
Other audio editing commands keep Audacity's usual selection behavior. Clipboard
copies retain group metadata.

The track menu adds **Add Audio Track Before This Track**,
**Add Audio Track After This Track**, and matching **Add Video Track** before/after
actions. The new empty audio track matches an audio
anchor's mono/stereo channel count; a non-audio anchor creates mono. Each
insertion is one Undo action. The same operations appear under Tracks > Add New,
using the focused track, and as `AddAudioTrackBefore: Track=0` and
`AddAudioTrackAfter: Track=0` scripting commands.

## Upstream integration points

The video timing model in `src/video/EditVideoTimeline.h` uses only standard C++
values. Its split/move/delete and source-time mapping tests run without Audacity
objects. `EditVideoTimeline.cpp` adapts that model to the native project XML,
Undo and command registries; `ShowVideoPreview.cpp` owns the player/clock and
window adapters. `ShowVideoTrack.cpp` owns the main-window filmstrip and mouse
drag adapter. It finds the ruler by its native object identity, then uses the
host's time/pixel mapping; a missing or ambiguous layout is rejected rather
than attaching the strip to an unrelated row. `PrepareVideoPreview.cpp` owns
the leased preview/thumbnail cache. `ExportVideoMix.cpp` adapts native audio export and translates
our picture timeline into FFmpeg filters. Export does not depend on a live
preview window or a native audio-clip implementation.

The video module uses the native importer, audio exporter and command registry.
Its small importer wrapper bypasses music tempo matching. Enabled video builds
keep the numeric runtime locale at C in Windows language setup, as required by
libmpv; the native display separator is captured first.

Clip group storage and commands belong to `src/clip-groups/GroupAudioClips.cpp`;
no grouping API is added to the upstream wave-track utility library. Clip
attachments preserve metadata through XML and Undo copies. The necessary XML
reader hook builds attachment factories before dispatching attributes. The
native movement collector calls one group-expansion adapter, then retains its
ordinary collision rules. Track insertion is a shared native adapter around the
track factory, insertion, focus and history APIs, used by both menu entry points.

The scripting fixes keep command construction and project destruction on the GUI
thread, avoid redrawing closed projects, and defer Quit until its command returns.
Branding keeps upstream internal class and build-target names. The bundled FFmpeg
DLL names identify the supported ABI in the video CMake module. These are the
specific integration points affected by corresponding upstream API changes.

The feature tests use public scripting commands and inspect resulting projects,
audio and video. They do not assert upstream source text, minified names or
unrelated window positions. Changes to a required upstream contract can still
require repair in its adapter; the owned video timing model and its tests have
no dependency on those contracts.
