# Pinned MIT speech worker. It has no links to Audacity or its module ABI.
# Models are downloaded only by an explicit caption operation, never by startup.
function(autopsy_build_speech_engine)
   include(FetchContent)
   set(BUILD_SHARED_LIBS OFF)
   set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
   set(WHISPER_BUILD_EXAMPLES ON CACHE BOOL "" FORCE)
   set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
   set(WHISPER_INSTALL OFF CACHE BOOL "" FORCE)
   set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
   set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
   set(GGML_AVX OFF CACHE BOOL "" FORCE)
   set(GGML_AVX2 OFF CACHE BOOL "" FORCE)
   set(GGML_FMA OFF CACHE BOOL "" FORCE)
   set(GGML_F16C OFF CACHE BOOL "" FORCE)
   set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
   set(GGML_CUDA OFF CACHE BOOL "" FORCE)
   set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
   set(GGML_METAL OFF CACHE BOOL "" FORCE)
   FetchContent_Declare(autopsy_whisper
      URL https://codeload.github.com/ggml-org/whisper.cpp/tar.gz/d1be6fde11ac6e0407606b4e42fe72d34add8037
      URL_HASH SHA256=645445da884df6dbd52b30f82d7be7c01d1767c938714e477e786b88d529e9f4
      EXCLUDE_FROM_ALL)
   FetchContent_MakeAvailable(autopsy_whisper)
   set_target_properties(whisper-cli PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_speech-worker")
   get_target_property(worker_directory Audacity RUNTIME_OUTPUT_DIRECTORY)
   if(CMAKE_CONFIGURATION_TYPES)
      string(APPEND worker_directory "/$<CONFIG>")
   endif()
   add_custom_target(copy-caption-worker
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${worker_directory}"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:whisper-cli>"
         "${worker_directory}/autopsy-whisper.exe"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${autopsy_whisper_SOURCE_DIR}/LICENSE"
         "${worker_directory}/whisper-LICENSE.txt"
      DEPENDS whisper-cli VERBATIM)
   add_dependencies(Audacity copy-caption-worker)
endfunction()
autopsy_build_speech_engine()
