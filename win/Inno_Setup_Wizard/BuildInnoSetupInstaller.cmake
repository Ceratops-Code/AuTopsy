# Build the AuTopsy installer. Package is private staging, never a deployment.
# Required parameters:
# BUILD_DIR - should be set to CMAKE_BINARY_DIR by the caller
# SOURCE_DIR - should be set to CMAKE_SOURCE_DIR by the caller
# OUTPUT_DIR - directory, where installer will be built
# INNO_SETUP_COMPILER - InnoSetup compiler executable
# BUILDING_64_BIT - Flag, that indicates that we are building a 64-bit installer
# BUILDING_ARM64 - Flag, that indicates that we are building an ARM64 installer
# EMBED_MANUAL - embed a fresh copy of manual
# SIGN - sign the installer
# USE_GPL3 - set the license to GPLv3 in the installer

if(BUILDING_ARM64)
    set(INSTALLER_SUFFIX "arm64")
    set(INSTALLER_X64_MODE "ArchitecturesInstallIn64BitMode=arm64")
elseif(BUILDING_64_BIT)
    set(INSTALLER_SUFFIX "x64")
    set(INSTALLER_X64_MODE "ArchitecturesAllowed=x64compatible\nArchitecturesInstallIn64BitMode=x64compatible")
else()
    set(INSTALLER_SUFFIX "x86")
    set(INSTALLER_X64_MODE "")
endif()

if( SIGN )
    set( SIGN_TOOL "SignTool=byparam powershell -ExecutionPolicy Bypass -File \$q${SOURCE_DIR}/scripts/build/windows/PfxSign.ps1\$q -File $f")
else()
    set( SIGN_TOOL )
endif()

if( EMBED_MANUAL )
    set ( MANUAL [[Source: "Package\help\manual\*"; DestDir: "{app}\help\manual\"; Flags: ignoreversion recursesubdirs]])
else()
    set( MANUAL )
endif()

if( USE_GPL3 )
   set( GPL_VERSION 3 )
   set( GPL_URL "https://www.gnu.org/licenses/gpl-3.0.en.html")
else()
set( GPL_VERSION 2 )
set( GPL_URL "http://www.gnu.org/licenses/old-licenses/gpl-2.0.html")
endif()

# OUTPUT_DIR owns one disposable staging tree. A fresh build must not package
# leftovers from an earlier successful build or continue after staging fails.
get_filename_component(_build_root "${BUILD_DIR}" ABSOLUTE)
get_filename_component(_staging_root "${OUTPUT_DIR}" ABSOLUTE)
if(NOT _staging_root STREQUAL "${_build_root}/innosetup")
    message(FATAL_ERROR "Installer staging must be BUILD_DIR/innosetup")
endif()
file(LOCK "${_build_root}/_installer.lock" GUARD PROCESS TIMEOUT 0 RESULT_VARIABLE _lock_result)
if(NOT _lock_result EQUAL 0)
    message(FATAL_ERROR "Another installer build owns this build directory: ${_lock_result}")
endif()
file(REMOVE_RECURSE "${_staging_root}")
file(MAKE_DIRECTORY "${_staging_root}")

# Only this producer's installer group is bounded. Protect the just-published
# artifact and keep at most two predecessors; never prune unrelated packages.
function(retain_installer_artifacts current)
    file(GLOB _installers "${_build_root}/package/autopsy-win-*-${INSTALLER_SUFFIX}.exe")
    list(REMOVE_ITEM _installers "${current}")
    set(_ordered)
    foreach(_installer IN LISTS _installers)
        file(TIMESTAMP "${_installer}" _time "%Y%m%d%H%M%S" UTC)
        list(APPEND _ordered "${_time}|${_installer}")
    endforeach()
    list(SORT _ordered ORDER DESCENDING)
    set(_keep 3)
    if(NOT current STREQUAL "")
        set(_keep 2)
    endif()
    set(_index 0)
    foreach(_record IN LISTS _ordered)
        if(_index GREATER_EQUAL _keep)
            string(REGEX REPLACE "^[^|]*[|]" "" _obsolete "${_record}")
            file(REMOVE "${_obsolete}")
        endif()
        math(EXPR _index "${_index} + 1")
    endforeach()
endfunction()
retain_installer_artifacts("")
set(_pending_installer "${_build_root}/package/_autopsy-installer-${INSTALLER_SUFFIX}.tmp")
file(REMOVE "${_pending_installer}")

file(COPY "${SOURCE_DIR}/win/Inno_Setup_Wizard/" DESTINATION "${OUTPUT_DIR}")
configure_file("${OUTPUT_DIR}/audacity.iss.in" "${OUTPUT_DIR}/audacity.iss")
configure_file("${OUTPUT_DIR}/audacity_InnoWizard_InfoBefore.rtf.in" "${OUTPUT_DIR}/audacity_InnoWizard_InfoBefore.rtf")

# Copy additional files

file(COPY
        "${SOURCE_DIR}/LICENSE.txt"
        "${SOURCE_DIR}/win/audacity.ico"
    DESTINATION
        "${OUTPUT_DIR}/Additional"
)

# "Install" prebuilt package

execute_process(
    COMMAND
        ${CMAKE_COMMAND}
            --install ${BUILD_DIR}
            --prefix "${OUTPUT_DIR}/Package"
            --config "${CONFIG}"
    RESULT_VARIABLE STAGING_RESULT
)
if(NOT STAGING_RESULT EQUAL 0)
    file(REMOVE_RECURSE "${_staging_root}")
    message(FATAL_ERROR "Installer staging failed (${STAGING_RESULT}); no installer produced")
endif()

# Build the installer

execute_process(
    COMMAND
        ${INNO_SETUP_COMPILER} /Sbyparam=$p "audacity.iss"
    WORKING_DIRECTORY
        ${OUTPUT_DIR}
    RESULT_VARIABLE
        ISCC_RESULT
)

if( NOT ISCC_RESULT EQUAL 0 )
    file(REMOVE_RECURSE "${_staging_root}")
    message( FATAL_ERROR "Inno Setup compiler failed (${ISCC_RESULT})" )
endif()

# Publish only a completed installer under an immutable content identity. A
# stopped copy leaves a private temporary file that the next build removes.
file(GLOB _completed "${OUTPUT_DIR}/Output/autopsy-win-*-${INSTALLER_SUFFIX}.exe")
list(LENGTH _completed _count)
if(NOT _count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one completed AuTopsy installer")
endif()
list(GET _completed 0 _installer)
file(SHA256 "${_installer}" _digest)
string(SUBSTRING "${_digest}" 0 12 _identity)
get_filename_component(_name "${_installer}" NAME)
string(REPLACE "-${INSTALLER_SUFFIX}.exe" "-${_identity}-${INSTALLER_SUFFIX}.exe" _name "${_name}")
file(MAKE_DIRECTORY "${_build_root}/package")
set(_published "${_build_root}/package/${_name}")
if(EXISTS "${_published}")
    file(SHA256 "${_published}" _existing_digest)
    if(NOT _existing_digest STREQUAL _digest)
        message(FATAL_ERROR "Existing installer identity has different contents")
    endif()
else()
    configure_file("${_installer}" "${_pending_installer}" COPYONLY)
    file(RENAME "${_pending_installer}" "${_published}")
endif()
retain_installer_artifacts("${_published}")
message(STATUS "AuTopsy installer: ${_published}")
message(STATUS "SHA256: ${_digest}")
# Staging contains runtime files, which are not deliverable artifacts.
file(REMOVE_RECURSE "${_staging_root}")
