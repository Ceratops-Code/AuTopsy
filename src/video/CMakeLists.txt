# One source owner for standalone checks and the host's native test targets.
set(autopsy_video_playback_test_sources EditVideoContent.cpp JoinVideoClips.cpp TestVideoPlayback.cpp)
set(autopsy_video_edits_test_sources EditVideoContent.cpp JoinVideoClips.cpp PlanVideoEdits.cpp TestVideoEdits.cpp)
set(autopsy_video_composition_test_sources EditVideoContent.cpp JoinVideoClips.cpp PlanVideoEdits.cpp
   EvaluateVideoTimeline.cpp RenderVideoComposition.cpp PlanVideoExport.cpp TestVideoComposition.cpp)

#[[
AuTopsy video tracks and synchronized preview. The SDK is an external libmpv development
package; its DLL stays beside this build's executable, never in system folders.
]]

# The owned timing/transport models can be checked without configuring the
# upstream application, downloading its audio SDKs, or opening a desktop window.
# Both CI and local contributors use this same CMake/CTest entry point.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   cmake_minimum_required(VERSION 3.16)
   project(AuTopsyVideoCoreChecks LANGUAGES CXX)
   enable_testing()
   # An explicit native session is a separate test scope. Reconfiguration
   # without these inputs restores the portable source checks for CI/promotion.
   if(DEFINED ENV{AUTOPSY_VIDEO_BUILD_DIR} AND DEFINED ENV{AUTOPSY_VIDEO_TEST_NAME})
      add_test(NAME fork-native-media-session COMMAND uv run --project
         "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts" --locked --with cmake==3.31.10
         python "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build/windows/build_video_installer.py"
         --test-existing-build --build-dir "$ENV{AUTOPSY_VIDEO_BUILD_DIR}"
         --test-name "$ENV{AUTOPSY_VIDEO_TEST_NAME}")
      set_tests_properties(fork-native-media-session PROPERTIES RUN_SERIAL TRUE TIMEOUT 660)
      return()
   endif()
   include(FetchContent)
   FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG 6bf41f4f71bf25641befe33365b9d3da4d940920)
   FetchContent_MakeAvailable(Catch2)
   include("${catch2_SOURCE_DIR}/contrib/Catch.cmake")
   add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/lib-project-file-io/tests"
      "${CMAKE_BINARY_DIR}/project-storage")
   add_executable(media-clip-selection-test ../clip-groups/TestMediaClipSelection.cpp ../../tests/Catch2Main.cpp)
   target_compile_features(media-clip-selection-test PRIVATE cxx_std_17)
   target_link_libraries(media-clip-selection-test PRIVATE Catch2::Catch2)
   catch_discover_tests(media-clip-selection-test TEST_PREFIX "media-clip-selection::" PROPERTIES TIMEOUT 30)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../track-groups" "${CMAKE_BINARY_DIR}/track-groups")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../recording" "${CMAKE_BINARY_DIR}/recording")
   add_executable(video-playback-test ${autopsy_video_playback_test_sources} ../../tests/Catch2Main.cpp)
   target_compile_features(video-playback-test PRIVATE cxx_std_17)
   target_link_libraries(video-playback-test PRIVATE Catch2::Catch2)
   catch_discover_tests(video-playback-test TEST_PREFIX "video-playback::"
      PROPERTIES TIMEOUT 30)
   add_executable(video-edits-test ${autopsy_video_edits_test_sources} ../../tests/Catch2Main.cpp)
   target_compile_features(video-edits-test PRIVATE cxx_std_17)
   target_link_libraries(video-edits-test PRIVATE Catch2::Catch2)
   catch_discover_tests(video-edits-test TEST_PREFIX "video-edits::" PROPERTIES TIMEOUT 30)
   add_executable(video-composition-test ${autopsy_video_composition_test_sources} ../../tests/Catch2Main.cpp)
   target_compile_features(video-composition-test PRIVATE cxx_std_17)
   target_link_libraries(video-composition-test PRIVATE Catch2::Catch2)
   catch_discover_tests(video-composition-test TEST_PREFIX "video-composition::" PROPERTIES TIMEOUT 30)
   add_executable(product-branding-core-test
      ../../libraries/lib-strings/tests/TestProductBranding.cpp ../../tests/Catch2Main.cpp)
   target_compile_features(product-branding-core-test PRIVATE cxx_std_17)
   target_compile_definitions(product-branding-core-test PRIVATE AUTOPSY_BRANDING_CORE_ONLY=1)
   target_link_libraries(product-branding-core-test PRIVATE Catch2::Catch2)
   catch_discover_tests(product-branding-core-test TEST_PREFIX "product-branding-core::"
      PROPERTIES TIMEOUT 30)
   find_package(Python3 QUIET COMPONENTS Interpreter)
   if(Python3_Interpreter_FOUND)
      file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_installer-tests" "${CMAKE_BINARY_DIR}/_selection-tests"
         "${CMAKE_BINARY_DIR}/_consumer-tests")
      add_test(NAME fork-audacity-consumer-runtime COMMAND uv run --project
         "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts" --locked python -m unittest discover
         -s "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/piped-work" -p test_run_audacity_consumer.py)
      set_tests_properties(fork-audacity-consumer-runtime PROPERTIES TIMEOUT 180
         ENVIRONMENT "AUTOPSY_TEST_TEMP_ROOT=${CMAKE_BINARY_DIR}/_consumer-tests")
      add_test(NAME fork-audacity-consumer-cache COMMAND uv run --project
         "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts" --locked python -m unittest discover
         -s "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/piped-work" -p test_prepare_audacity_consumer.py)
      set_tests_properties(fork-audacity-consumer-cache PROPERTIES TIMEOUT 180
         ENVIRONMENT "AUTOPSY_TEST_TEMP_ROOT=${CMAKE_BINARY_DIR}/_consumer-tests")
      add_test(NAME affected-test-selection COMMAND uv run --project
         "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts" --locked python -m unittest discover
         -s "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts" -p test_run_affected_tests.py)
      set_tests_properties(affected-test-selection PROPERTIES TIMEOUT 180
         ENVIRONMENT "AUTOPSY_TEST_TEMP_ROOT=${CMAKE_BINARY_DIR}/_selection-tests")
      add_test(NAME fork-installer-build COMMAND uv run --project
         "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts" --locked python -m unittest discover
         -s "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build/windows" -p test_build_video_installer.py)
      set_tests_properties(fork-installer-build PROPERTIES
         ENVIRONMENT "AUTOPSY_TEST_TEMP_ROOT=${CMAKE_BINARY_DIR}/_installer-tests")
      add_test(NAME fork-display-policy COMMAND "${Python3_EXECUTABLE}"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/piped-work/test_fork_features.py"
         --case display-policy)
      add_test(NAME fork-worker-lifecycle COMMAND "${Python3_EXECUTABLE}"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/piped-work/test_fork_features.py"
         --case worker-lifecycle --work-root "${CMAKE_BINARY_DIR}/_fork-tests")
   endif()
   return()
endif()

option(audacity_has_video_preview "Build video preview and clip editing" OFF)
if(NOT audacity_has_video_preview)
   return()
endif()

set(audacity_video_mpv_sdk "" CACHE PATH "Extracted libmpv development package")
find_path(audacity_video_mpv_include mpv/client.h
   HINTS "${audacity_video_mpv_sdk}/include")
find_file(audacity_video_mpv_library libmpv-2.dll
   HINTS "${audacity_video_mpv_sdk}" NO_DEFAULT_PATH)
if(NOT audacity_video_mpv_include OR NOT audacity_video_mpv_library)
   message(FATAL_ERROR
      "Video preview needs an extracted libmpv SDK. Set audacity_video_mpv_sdk "
      "to the directory containing include/mpv/client.h and libmpv-2.dll, "
      "or set audacity_has_video_preview=OFF.")
endif()

# The normal Audacity FFmpeg importer decodes the soundtrack directly into
# project audio. Keep its compatible shared runtime beside the executable.
set(audacity_video_ffmpeg_runtime "" CACHE PATH "FFmpeg 8.1 shared runtime directory")
if(audacity_use_ffmpeg STREQUAL "off")
   message(FATAL_ERROR "Video soundtrack import requires audacity_use_ffmpeg=loaded.")
endif()
set(_video_ffmpeg_libraries)
foreach(_video_ffmpeg_name avformat-62.dll avcodec-62.dll avutil-60.dll
   swresample-6.dll swscale-9.dll avfilter-11.dll avdevice-62.dll ffmpeg.exe ffprobe.exe)
   set(_video_ffmpeg_file "${audacity_video_ffmpeg_runtime}/${_video_ffmpeg_name}")
   if(NOT EXISTS "${_video_ffmpeg_file}")
      message(FATAL_ERROR
         "Video soundtrack import needs FFmpeg 8.1 shared libraries. Set "
         "audacity_video_ffmpeg_runtime to their directory (missing ${_video_ffmpeg_name}).")
   endif()
   list(APPEND _video_ffmpeg_libraries "${_video_ffmpeg_file}")
endforeach()
if(NOT EXISTS "${audacity_video_ffmpeg_runtime}/LICENSE.txt")
   message(FATAL_ERROR "Keep the FFmpeg distribution LICENSE.txt with its shared runtime.")
endif()

target_sources(Audacity PRIVATE EditVideoContent.cpp EditVideoContent.h JoinVideoClips.cpp JoinVideoClips.h
   ShowVideoPreview.cpp SyncVideoPlayback.h PlayVideoMedia.cpp PlayVideoMedia.h
   ExportVideoMix.cpp ExportVideoMix.h EditVideoTimeline.cpp EditVideoTimeline.h
   PlanVideoEdits.cpp PlanVideoEdits.h EvaluateVideoTimeline.cpp EvaluateVideoTimeline.h
   RenderVideoComposition.cpp RenderVideoComposition.h EditVideoComposition.cpp EditVideoComposition.h
   PlanVideoExport.cpp PlanVideoExport.h
   PrepareVideoPreview.cpp PrepareVideoPreview.h ShowVideoTrack.cpp ShowVideoTrack.h
   CreateVideoTrack.cpp CreateVideoTrack.h ChooseVideoExport.cpp ChooseVideoExport.h RelinkVideoMedia.cpp RelinkVideoMedia.h
   ../clip-groups/LinkMediaClips.cpp ../clip-groups/LinkMediaClips.h
   ../clip-groups/EditClipMarkers.cpp ../clip-groups/EditClipMarkers.h)
target_include_directories(Audacity PRIVATE "${audacity_video_mpv_include}")
# Only the enabled video build changes the numeric-runtime requirement. Keep
# this definition on the owning translation unit rather than every app source.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../prefs/GUISettings.cpp"
   TARGET_DIRECTORY Audacity APPEND PROPERTY COMPILE_DEFINITIONS
   AUTOPSY_HAS_VIDEO_PREVIEW=1)
set_property(SOURCE
   "${CMAKE_CURRENT_SOURCE_DIR}/../menus/EditMenus.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../clip-groups/GroupAudioClips.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../clip-groups/SelectMediaClips.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/../clip-groups/OpenClipActions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tracks/playabletrack/wavetrack/ui/WaveClipAdjustBorderHandle.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tracks/playabletrack/wavetrack/ui/WaveTrackAffordanceControls.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tracks/ui/CommonTrackControls.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ProjectFileManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ProjectAudioManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../TransportUtilities.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../toolbars/ControlToolBar.cpp"
   TARGET_DIRECTORY Audacity APPEND PROPERTY COMPILE_DEFINITIONS
   AUTOPSY_HAS_VIDEO_PREVIEW=1)

# A dependency target runs even when the executable needs no relink, restoring
# a removed runtime and applying a replacement SDK on every app build.
get_target_property(_video_executable_dir Audacity RUNTIME_OUTPUT_DIRECTORY)
if(CMAKE_CONFIGURATION_TYPES)
   string(APPEND _video_executable_dir "/$<CONFIG>")
endif()
add_custom_target(copy-video-player
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${_video_executable_dir}"
   COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${audacity_video_mpv_library}" "${_video_executable_dir}/libmpv-2.dll"
   COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      ${_video_ffmpeg_libraries} "${_video_executable_dir}"
   COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${audacity_video_ffmpeg_runtime}/LICENSE.txt"
      "${_video_executable_dir}/FFmpeg-LICENSE.txt"
   VERBATIM)
add_dependencies(Audacity copy-video-player)

add_unit_test(
   NAME video-playback
   SOURCES ${autopsy_video_playback_test_sources}
)
add_unit_test(NAME video-edits SOURCES ${autopsy_video_edits_test_sources})
add_unit_test(NAME video-composition SOURCES ${autopsy_video_composition_test_sources})
add_unit_test(NAME media-clip-selection SOURCES ../clip-groups/TestMediaClipSelection.cpp)
if(TARGET video-playback-test)
   add_test(NAME video-timeline COMMAND video-playback-test "[video-timeline]")
   find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_installer-tests" "${CMAKE_BINARY_DIR}/_consumer-tests")
      add_test(NAME fork-audacity-consumer-runtime COMMAND uv run --project
         "${CMAKE_SOURCE_DIR}/scripts" --locked python -m unittest discover
         -s "${CMAKE_SOURCE_DIR}/scripts/piped-work" -p test_run_audacity_consumer.py)
      set_tests_properties(fork-audacity-consumer-runtime PROPERTIES TIMEOUT 180
         ENVIRONMENT "AUTOPSY_TEST_TEMP_ROOT=${CMAKE_BINARY_DIR}/_consumer-tests")
      add_test(NAME fork-audacity-consumer-cache COMMAND uv run --project
         "${CMAKE_SOURCE_DIR}/scripts" --locked python -m unittest discover
         -s "${CMAKE_SOURCE_DIR}/scripts/piped-work" -p test_prepare_audacity_consumer.py)
      set_tests_properties(fork-audacity-consumer-cache PROPERTIES TIMEOUT 180
         ENVIRONMENT "AUTOPSY_TEST_TEMP_ROOT=${CMAKE_BINARY_DIR}/_consumer-tests")
      add_test(NAME fork-installer-build COMMAND uv run --project
         "${CMAKE_SOURCE_DIR}/scripts" --locked python -m unittest discover
         -s "${CMAKE_SOURCE_DIR}/scripts/build/windows" -p test_build_video_installer.py)
      set_tests_properties(fork-installer-build PROPERTIES
         ENVIRONMENT "AUTOPSY_TEST_TEMP_ROOT=${CMAKE_BINARY_DIR}/_installer-tests")
       add_test(NAME fork-display-policy COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/scripts/piped-work/test_fork_features.py" --case display-policy)
       add_test(NAME fork-worker-lifecycle COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/scripts/piped-work/test_fork_features.py"
          --case worker-lifecycle --work-root "${CMAKE_BINARY_DIR}/_fork-tests")
       add_test(NAME fork-display-rejection COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/scripts/piped-work/test_fork_features.py"
          --executable "$<TARGET_FILE:Audacity>" --work-root "${CMAKE_BINARY_DIR}/_fork-tests"
          --case branding --reject-display)
       set_tests_properties(fork-display-rejection PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
        foreach(_case clip-selection clip-actions video-clip-export video-clip-mute edit-cursor-line groups tracks video video-selection video-edges video-linked-edges video-split-edges video-linked-split-edges video-clip-edits video-linked-clip-edits video-compound-render video-crop video-crop-controls video-export-timing video-transport video-visibility video-speed video-speed-storage video-clip-storage video-parent-links video-clipboard video-track-menu media preview branding fixtures history-view transport-toolbar preview-navigation project-compatibility)
         add_test(NAME fork-${_case}
            COMMAND "${Python3_EXECUTABLE}"
               "${CMAKE_SOURCE_DIR}/scripts/piped-work/test_fork_features.py"
               --executable "$<TARGET_FILE:Audacity>"
               --work-root "${CMAKE_BINARY_DIR}/_fork-tests"
               --case "${_case}"
               --ffmpeg "${audacity_video_ffmpeg_runtime}/ffmpeg.exe"
               --ffprobe "${audacity_video_ffmpeg_runtime}/ffprobe.exe")
         set_tests_properties(fork-${_case} PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
      endforeach()
      set_tests_properties(fork-project-compatibility PROPERTIES TIMEOUT 600)
   endif()
endif()
