# AuTopsy backlog

This is the product backlog and operation inventory for audio/video editing.
It includes the requested audio-clip operations and meaningful video equivalents,
not just the next release. The architecture and delivery order are in
[the design](docs/design.md). A planned item is not an available feature.

## Current delivery

- [Preview v3.7.10-autopsy.2](https://github.com/Ceratops-Code/AuTopsy/releases/tag/v3.7.10-autopsy.2)
  is public. Its unsigned Windows installer was built on GitHub-hosted runners.
- Native video tracks share the main scrolling pane with audio, support several
  sources, and have filmstrip clips. Video import creates separate soundtrack
  tracks, linked to the picture by default.
- Move, split, edge trim/untrim, basic range/clipboard edits, track insertion,
  project persistence, Undo/Redo, clip flags, media relinking, frame/timecode
  navigation and modern export presets are implemented in that source.
- The two newest native regressions passed locally. The broader desktop media
  case stopped at an existing held-mouse capture step; it is not a full desktop
  test pass. NVIDIA export and the performance targets below remain unmeasured.
- No new installer was run on the user's occupied installation. Test footage is
  generated colour patterns and a tone; personal media is excluded.

## Foundation progress

The first architecture step is implemented in the task branch: the pure model
header has no native project declarations, and one owned layer evaluator drives
both preview picture lookup and export composition. Independent cases cover the
same source frames after layered moves, trims and gaps at different frame rates.
The portable feature suites and repository source checks passed. A new full native
application build and desktop session have not been run for this foundation change.
The content/instance model, transactional planner and new user controls remain
future work; this step does not claim those features are delivered.

## Release follow-up: signing

The public preview, README signing policy and GitHub-hosted Windows installer
build are now in place. Start at [SignPath Foundation's application](https://signpath.org/apply).
Use the public AuTopsy repository, release and hosted build as project evidence;
the policy names RomanOstr as author, reviewer and signing approver.

Prepared project description: **AuTopsy is an independent GPLv3 fork of Audacity,
adding native multitrack video editing synchronized with separately editable
audio. It preserves upstream Git history and attribution. Windows installers
are built on GitHub-hosted runners and released publicly with a source/build
manifest. The maintainer reviews changes and manually approves release signing.**

Before submission, the maintainer must confirm MFA on the relevant accounts and
the applicable upstream signed-release condition in the Foundation's current
terms. Sole-maintainer acceptance is the Foundation's decision. No application
has been submitted and no sponsorship or certificate has been granted.

After acceptance: connect the scoped GitHub App, configure the project/artifact
and signing policy, store the service token in protected GitHub secrets, and
add the service-approved signing step after artifact upload. Sign owned binaries
before installer packaging and then sign the installer. Do not re-sign inherited
third-party binaries as our own. Add the required provider credit only once
sponsorship is granted. See the [terms](https://signpath.org/terms) and
[GitHub build integration](https://docs.signpath.io/trusted-build-systems/github).

## Reading the inventory

**Available** describes an existing operation, not a promise of every possible
audio/video combination. **Partial** needs the listed parity or integration work.
**Planned** has no completed user-facing implementation. Audio remains editable
through the inherited engine; picture processing must never silently modify its
separate soundtrack. Each row includes persistence, Undo/Redo, keyboard/menu and
scripting support where relevant, and linked-clip behavior.

## Selection, placement and navigation

| Operation | Video counterpart and expected behavior | State |
| --- | --- | --- |
| Select a clip | Select the picture clip in the main track pane | Available |
| Select several clips/tracks | Explicit clip sets, additive selection, all clips and time-range selection | Partial: native track/time selection exists; richer clip sets planned |
| Select content or edges | Select clip bounds, cursor to start/end, extend selection to edge | Partial |
| Horizontal move | Drag a clip header; preserve source range | Available |
| Numeric placement | Enter an exact start/timecode; absolute movement is repeatable | Partial: scripting exists; consistent property editor planned |
| Small/large nudge | Nudge by one frame or a configured interval with keyboard | Planned |
| Move to another track | Keep the exact timestamp if the destination has space | Available |
| Reorder clips | Rearrange pieces on one or several tracks without hidden overwrites | Partial |
| Align | Align selected starts/ends, cursor, selection and neighbouring edges | Planned |
| Snap | Source/project frames, playhead, markers and neighbouring edges; bypass modifier | Partial: source-frame snapping exists |
| Negative project time | Define picture behavior before zero and audio pre-roll consistently | Planned: current picture clips reject negative starts |
| Seek and scrub | Seek in either window; eventually smooth forward/backward scrubbing | Partial: normal seeks exist; scrub/variable-rate playback planned |
| Frame navigation | Previous/next original source frame and project-frame stepping | Partial: source stepping exists; project-frame mode planned |
| Timecode | Numeric HH:MM:SS:FF and fractional rates; explicit drop-frame mode if added | Partial: non-drop source timecode exists |
| Zoom/fit/scroll | Fit clip/selection/project; zoom at cursor; follow playhead | Partial: inherited pane behavior; picture-specific commands planned |

## Clip boundaries and structure

| Operation | Video counterpart and expected behavior | State |
| --- | --- | --- |
| Trim left/right edge | Hide footage without destroying the original source | Available |
| Untrim left/right edge | Drag back to reveal retained footage, bounded by source and neighbours | Available for simple source clips |
| Numeric trim | Set source in/out or project bounds precisely | Partial: common model exists; complete editor planned |
| Trim to selection | Keep the selected picture range and its linked sound | Available for current simple clips |
| Split | Split at cursor or selection edges; preserve frame mapping, flags and links | Available |
| Split new | Move selected picture into a new native track at its original time | Available |
| Heal an accidental split | Join continuous compatible pieces without Undo or re-encoding | Planned, first vertical slice |
| General join | Combine different sources/edits into one editable compound clip | Planned, first vertical slice |
| Join across gaps | Preserve the intervening blank interval; never resurrect deleted footage | Planned |
| Separate/disjoin | Recover constituent clips from a compound; retain placement and properties | Planned |
| Slip | Change source in/out while keeping project position and duration fixed | Planned |
| Slide | Move a clip while trimming its neighbours to keep the outer span fixed | Planned |
| Roll boundary | Move a shared cut while retaining the combined duration | Planned |
| Extend to neighbour | Reveal available handles up to the adjacent clip without crossing it | Planned |
| Freeze/hold frame | Repeat a chosen frame for a duration; optional editable freeze clip | Planned |
| Replace clip content | Replace source/selected content while preserving declared placement/properties | Planned |

## Clipboard, deletion and time insertion

| Operation | Video counterpart and expected behavior | State |
| --- | --- | --- |
| Cut / copy / paste | Carry source references, trimmed ranges, links and flags across video tracks | Available for current clips |
| Split cut / split delete | Remove a range and leave a gap | Partial: pure range model exists; full menu parity planned |
| Ripple cut / delete | Close a gap and move affected later pieces according to explicit scope | Partial: native clear closes selected-track time; coordinated ripple planned |
| Delete whole clip | Remove picture independently when unlinked; preserve unrelated tracks | Available |
| Insert time / silence | Insert a black/transparent gap, moving later clips | Partial: native insertion model exists; full coordinated UI planned |
| Paste insert | Insert duration before placing copied picture | Available |
| Paste replace / overwrite | Replace only the chosen interval with a declared collision policy | Planned |
| Duplicate | New independent clip identities and independent copies of linked sets | Available |
| Repeat | Repeat a clip/range N times with correct flags and source mapping | Planned |
| Cross-project paste | Resolve sources, new identities and links without collisions or extra sound import | Partial: clipboard adapter exists; full contract cases planned |
| Undo / redo | One history operation for each complete mixed-media edit | Available for current edits; mandatory for every new operation |
| Recover interrupted edit | Publish all changed tracks or none; keep the previous saved project usable | Ongoing requirement |

## Clip metadata and relationships

| Operation | Video counterpart and expected behavior | State |
| --- | --- | --- |
| Clip name and colour | Per-clip names/colours independent of track name | Planned |
| Persistent group | Move selected audio/video pieces as one set; explicit ungroup | Partial: audio groups and mixed picture/sound links exist |
| Link picture and sound | Import linked, move/split/trim together; unlink and relink explicitly | Available |
| Temporarily ignore links | Edit one member with a clear modifier/command | Planned |
| Lock clip / track | Block edits and ripple through locked content; show a useful reason | Planned |
| Sync lock | Preserve chosen relative timing during insert/delete, including unselected linked tracks | Planned |
| Clip-local flag | Text flag anchored inside content, moves with the clip | Available |
| Flag lifecycle | Split/copy/compound/repeat/reverse/stretch map flags predictably | Partial: simple clips covered; compound and retiming cases planned |
| Flag navigation/editing | Rename, delete, jump, list, filter and optional colour | Partial: add/remove exists; richer navigation planned |
| Label-region operations | Apply edits to labelled ranges, while distinguishing project labels from clip flags | Planned |
| Save / reopen | Retain all editable properties, source references, IDs and relationships | Available for current features; mandatory for new model |

## Timing, interpretation and format

| Audio operation | Meaningful picture operation | State |
| --- | --- | --- |
| Change speed | Change playback speed and duration through an explicit source-time map | Planned |
| Stretch to duration/edge | Drag with a dedicated modifier or enter duration; ordinary edge drag remains trim | Planned |
| Tempo matching | Optional picture retiming to a declared duration or beat grid | Planned; never automatic on video import |
| Variable speed / ramp | Piecewise speed curve with explicit source mapping and preview support | Planned after constant-speed support |
| Reverse | Reverse selected picture; linked sound policy explicit | Planned |
| Repeat / loop | Repeat source occurrences without copying media; markers distinguish occurrences | Planned |
| Change sample-rate interpretation | Interpret source frame rate, changing duration without inventing frames | Planned |
| Resample | Convert output frame rate while preserving duration; choose sampling/interpolation method | Partial: export FPS selection exists; full clip/project policy planned |
| Bit depth / sample format | Choose picture pixel format, bit depth, colour space/range and alpha where supported | Planned |
| Render / mix down | Render selected picture layers or a compound to a new source; original edits retained unless explicitly replaced | Planned |
| Non-destructive properties | Save timing/effects independently of the original file | Partial: placement and trim exist |

Pitch, gain, pan, loudness normalization, audio channel mixing, sample repair,
noise reduction and audio-only generators do not act on picture. They remain
available on the separately imported soundtrack. Video frame rate is a timing
quantity; codec bit rate is an output quality/storage quantity, not its equivalent.

## Visibility, composition and preview transport

| Operation | Expected behavior | State |
| --- | --- | --- |
| Mute video track | Hide that layer in preview and export; lower eligible layers show through | Partial: track-menu Show/Hide exists; visible Mute button planned |
| Solo video track | Restrict eligible picture layers to soloed tracks; mute wins | Planned |
| Mute video clip | Hide only that occurrence without deleting it or muting its separate audio | Planned |
| Track order | Upper eligible picture wins; reorder using native controls | Available |
| All picture muted | Black output for the unchanged project duration, with edited audio still audible | Planned: explicit canvas/duration required |
| Preview Play / Pause / Stop | Buttons in preview control the same project audio clock as main transport | Planned, first vertical slice |
| Loop / play selection | Preview follows selection loops and jumps; optional dedicated controls | Partial: ordinary transport loops exist |
| Focus and shortcuts | Space and navigation work consistently in both windows without double-triggering | Planned |
| Accurate still / smooth playback | Original source for frame inspection; low-resolution proxy for motion | Available for ordinary speed; measured targets remain open |
| Preview quality | Choose resolution/quality without changing export or source | Partial: automatic proxy exists; user controls planned |
| Background preparation | Cancellable progress, keep editing during proxy generation, safe stale-worker discard | Planned |

## Picture processing, output and media management

| Operation | Expected behavior | State |
| --- | --- | --- |
| Fade in/out | Opacity fades reveal lower layers or background | Planned |
| Crossfade | Dissolve between overlapping pictures; explicit overlap rules | Planned |
| Envelope / automation | Keyframed opacity, transforms and later supported picture effects | Planned |
| Crop / scale / rotate / flip | Non-destructive framing, orientation and aspect controls | Planned |
| Colour / brightness / contrast | Clip/layer processing with explicit colour and pixel-format rules | Planned |
| Other effects | Picture-relevant filters with preview/export agreement; no fake audio-effect parity | Planned |
| Titles / captions / subtitles | Generated visual clips or dedicated overlays, with exportable timing | Planned |
| Blank / colour / image source | Black, colour, still image and freeze clips without audio dummy tracks | Planned |
| Scene / black / freeze detection | Picture analyses produce editable cuts/flags, not destructive edits | Planned |
| Export with edited audio | Entire composed picture plus project audio mix | Available |
| Export selection / regions | Explicit selected interval, labelled regions and multiple files | Planned |
| Modern presets | H.264, HEVC, AV1, size/FPS/quality; optional hardware encoders | Available; NVIDIA hardware remains untested |
| Collect project media | Copy referenced files into a portable project folder with explicit ownership | Planned |
| Relink media | Replace a missing/moved source while preserving edits, flags and links | Available |
| Missing-media workflow | Locate all missing files, batch relink, inspect metadata, retain offline edits | Partial |
| Proxy/cache management | Bounded owned cache, leases for live files, regenerate without touching originals | Available; background/quality UI planned |
| Commands/macros | Equivalent menu, keyboard and scripting operations with meaningful errors | Partial; required for each new operation |

## Delivery sequence and acceptance

1. Finish interrupted preview work and publication. Completed for v2; retain the
   documented desktop automation limit and inherited security findings.
2. Establish the owned editing foundation: isolate native adapters, shared
   evaluation, stable identities, content/instance separation, time mapping,
   transactional edit plans, project settings and versioned persistence.
3. Deliver a complete first slice: heal/join/compound, reversible edge behavior,
   clip/track mute and solo, preview Play/Pause/Stop, linked edits and flags.
4. Add constant retiming, reverse, repeat and frame-rate policy. Gate more complex
   retiming/effects on a measured smooth-preview approach.
5. Complete processing, media management and remaining operation parity above.

Every operation needs independent model behavior cases and a narrow native adapter
case, covering repeated calls, interrupted effects, failure without partial edits,
save/reopen and Undo/Redo. Keep native audio behavior and public file formats intact.
Changing an upstream interface should require adapting the boundary, not rebuilding
the video model. See [the test plan](docs/design.md#12-behavior-evidence-and-delivery).

The presence of this full inventory does not authorize installing over a running
session, processing personal media, automatic upstream merges or dismissing the
inherited findings.
