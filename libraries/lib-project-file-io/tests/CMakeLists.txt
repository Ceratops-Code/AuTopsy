# The same SQLite configuration code runs in the application and these tests.
# A private in-memory VFS models a power cut without files, windows, or devices.
if(TARGET Catch2::Catch2)
   enable_language(C)
   add_library(project-storage-sqlite STATIC ../../../lib-src/sqlite/sqlite3.c)
   target_include_directories(project-storage-sqlite PUBLIC ../../../lib-src/sqlite)
   find_package(Threads REQUIRED)
   target_link_libraries(project-storage-sqlite PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
   add_executable(project-storage-test ../ConfigureProjectStorage.cpp
      TestProjectStorage.cpp ../../../tests/Catch2Main.cpp)
   target_compile_features(project-storage-test PRIVATE cxx_std_17)
   target_link_libraries(project-storage-test PRIVATE Catch2::Catch2 project-storage-sqlite)
   catch_discover_tests(project-storage-test TEST_PREFIX "project-storage::"
      PROPERTIES TIMEOUT 60)
else()
   add_unit_test(NAME project-storage SOURCES ../ConfigureProjectStorage.cpp
      TestProjectStorage.cpp LIBRARIES lib-sqlite-helpers-interface)
endif()
