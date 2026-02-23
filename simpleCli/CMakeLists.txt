cmake_minimum_required(VERSION 3.10)

project(SimpleCli LANGUAGES CXX)

### Enumerate common files ###
set(_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/app)
set(_hdr_dir ${CMAKE_CURRENT_SOURCE_DIR}/app/CRSDK)
set(_lib_dir ${CMAKE_CURRENT_SOURCE_DIR}/external/crsdk)

find_package(Threads REQUIRED)

set(_common
    ${_src_dir}/CrDebugString.cpp
    ${_src_dir}/CrDebugString.h
    ${_src_dir}/httplib.h
    ${_hdr_dir}/CameraRemote_SDK.h
    ${_hdr_dir}/CrCommandData.h
    ${_hdr_dir}/CrDefines.h
    ${_hdr_dir}/CrDeviceProperty.h
    ${_hdr_dir}/CrError.h
    ${_hdr_dir}/CrImageDataBlock.h
    ${_hdr_dir}/CrTypes.h
    ${_hdr_dir}/ICrCameraObjectInfo.h
    ${_hdr_dir}/IDeviceCallback.h
)

### Define list of executables ###
set(cli_list
    afShooting
    connect
    connectMulti
    ContentsTransferMode
    controlFTPJob
    CreateCameraObjectInfo
    CustomGridLine
    downloadUploadSettingFile
    Eframing
    FTPServerSetting
    fx30MultiRecord
    getLensInformation
    getLiveViewAndOSD
    getLiveViewAndOSDhttp
    getSetDeviceProperty
    getSetDevicePropertyStr
    monitoring
    RemoteTransferMode
    reqOperation
)

list(GET cli_list 0 cli_first)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.1" CACHE STRING "Minimum OS X deployment version")
endif()

foreach(cli_name IN LISTS cli_list)
    add_executable(${cli_name}
        ${_src_dir}/${cli_name}.cpp
        ${_common}
    )

    target_include_directories(${cli_name} PRIVATE
        ${_hdr_dir}
    )

    find_library(_target_lib Cr_Core HINTS ${_lib_dir})
    target_link_libraries(${cli_name} PRIVATE
        ${_target_lib}
    )

    if(UNIX AND NOT APPLE)
        target_link_libraries(${cli_name} PRIVATE
            Threads::Threads
        )
    endif()

    if(APPLE AND "${cli_name}" STREQUAL "fx30MultiRecord")
        target_link_libraries(${cli_name} PRIVATE
            "-framework IOKit"
            "-framework CoreFoundation"
        )
    endif()

    if(APPLE)
        set_target_properties(${cli_name} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
            BUILD_RPATH "@executable_path"
        )
    else()
        set_target_properties(${cli_name} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN"
        )
    endif()

    ### OS specific configuration ###
    if(WIN32)
        target_compile_definitions(${cli_name} PRIVATE
            UNICODE _UNICODE
        )
        target_compile_options(${cli_name} PRIVATE
            -guard:cf
        )
        
    elseif(UNIX AND NOT APPLE)
        target_compile_options(${cli_name} PRIVATE
            -fstack-protector-all
            -fsigned-char
        )
        
        if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
            if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)
                # Must use std::experimental namespace if older than GCC8
                message("[${cli_name}] GCC version less than 8. Using std::experimental namespace.")
                target_compile_definitions(${cli_name} PRIVATE
                    USE_EXPERIMENTAL_FS)
            endif()

            if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
                # Must explicitly link separate std::filesystem if older than GCC9
                message("[${cli_name}] GCC version less than 9. Explicitly linking separate std::filesystem library.")
                target_link_libraries(${cli_name} PRIVATE
                    stdc++fs)
            endif()
        endif()
    endif()

    if("${cli_name}" STREQUAL "${cli_first}")
        ## Copy required library binaries
        if(WIN32)
            add_custom_command(TARGET ${cli_name} PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory ${_lib_dir} $<TARGET_FILE_DIR:${cli_name}>
            )
        elseif(UNIX AND NOT APPLE)
            add_custom_command(TARGET ${cli_name} PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory ${_lib_dir} $<TARGET_FILE_DIR:${cli_name}>
            )

            install(TARGETS ${cli_name} DESTINATION .)
            install(DIRECTORY ${_lib_dir}/ DESTINATION .)
        elseif(APPLE)
            set(ct_fw "Contents/Frameworks")
            set(adaptor_dir_name "CrAdapter")
            add_custom_command(TARGET ${cli_name} PRE_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${cli_name}>/${ct_fw}/${adaptor_dir_name}
                COMMAND ${CMAKE_COMMAND} -E copy_directory ${_lib_dir} $<TARGET_FILE_DIR:${cli_name}>
                COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:${cli_name}>/${adaptor_dir_name} $<TARGET_FILE_DIR:${cli_name}>/${ct_fw}/${adaptor_dir_name}
                COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:${cli_name}>/${adaptor_dir_name}
            )
        endif()
    else()
        add_dependencies(${cli_name} ${cli_first})
    endif()
endforeach()
