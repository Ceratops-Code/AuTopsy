# Pure folder/reduction policy is checked without configuring the editor.
set(_group_tests EditTrackGroups.cpp RenderGroupWaveform.cpp TestTrackGroups.cpp)
if(TARGET Audacity)
   target_sources(Audacity PRIVATE EditTrackGroups.h EditTrackGroups.cpp
      CreateTrackGroup.h CreateTrackGroup.cpp ShowTrackGroups.h ShowTrackGroups.cpp
      RenderGroupWaveform.h RenderGroupWaveform.cpp)
   target_link_libraries(Audacity PRIVATE lib-uuid lib-mixer)
   set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../tracks/ui/ChannelView.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/../TrackUtilities.cpp"
      TARGET_DIRECTORY Audacity APPEND PROPERTY COMPILE_DEFINITIONS AUTOPSY_HAS_TRACK_GROUPS=1)
   add_unit_test(NAME track-groups SOURCES ${_group_tests})
   if(WIN32 AND Python3_Interpreter_FOUND)
      add_test(NAME fork-track-groups COMMAND "${Python3_EXECUTABLE}"
         "${CMAKE_SOURCE_DIR}/scripts/piped-work/test_fork_features.py"
         --executable "$<TARGET_FILE:Audacity>" --work-root "${CMAKE_BINARY_DIR}/_fork-tests"
         --case track-groups --ffmpeg "${audacity_video_ffmpeg_runtime}/ffmpeg.exe"
         --ffprobe "${audacity_video_ffmpeg_runtime}/ffprobe.exe")
      set_tests_properties(fork-track-groups PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
   endif()
else()
   add_executable(track-groups-test ${_group_tests} ../../tests/Catch2Main.cpp)
   target_compile_features(track-groups-test PRIVATE cxx_std_17)
   target_link_libraries(track-groups-test PRIVATE Catch2::Catch2)
   catch_discover_tests(track-groups-test TEST_PREFIX "track-groups::" PROPERTIES TIMEOUT 30)
endif()
