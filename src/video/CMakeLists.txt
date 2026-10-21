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
   include(FetchContent)
   FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG 6bf41f4f71bf25641befe33365b9d3da4d940920)
   FetchContent_MakeAvailable(Catch2)
   add_executable(video-playback-test TestVideoPlayback.cpp ../../tests/Catch2Main.cpp)
   target_compile_features(video-playback-test PRIVATE cxx_std_17)
   target_link_libraries(video-playback-test PRIVATE Catch2::Catch2)
   add_test(NAME video-playback COMMAND video-playback-test)
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

target_sources(Audacity PRIVATE ShowVideoPreview.cpp SyncVideoPlayback.h
   ExportVideoMix.cpp ExportVideoMix.h EditVideoTimeline.cpp EditVideoTimeline.h
   PrepareVideoPreview.cpp PrepareVideoPreview.h ShowVideoTrack.cpp ShowVideoTrack.h
   CreateVideoTrack.cpp CreateVideoTrack.h)
target_include_directories(Audacity PRIVATE "${audacity_video_mpv_include}")
# Only the enabled video build changes the numeric-runtime requirement. Keep
# this definition on the owning translation unit rather than every app source.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../prefs/GUISettings.cpp"
   TARGET_DIRECTORY Audacity APPEND PROPERTY COMPILE_DEFINITIONS
   AUTOPSY_HAS_VIDEO_PREVIEW=1)
set_property(SOURCE
   "${CMAKE_CURRENT_SOURCE_DIR}/../menus/EditMenus.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tracks/ui/CommonTrackControls.cpp"
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
   SOURCES TestVideoPlayback.cpp
)
if(TARGET video-playback-test)
   add_test(NAME video-timeline COMMAND video-playback-test "[video-timeline]")
   find_package(Python3 QUIET COMPONENTS Interpreter)
   if(Python3_Interpreter_FOUND)
      foreach(_case groups tracks video preview branding)
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
   endif()
endif()
