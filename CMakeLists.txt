cmake_minimum_required(VERSION 3.16)

project(Peare VERSION 1.0.0 LANGUAGES C CXX)


# Peare sources retain a C++11 baseline on Qt 5 desktop targets.
set(PEARE_CXX_STANDARD "AUTO" CACHE STRING "C++ standard: AUTO, 11 or 17")
set_property(CACHE PEARE_CXX_STANDARD PROPERTY STRINGS AUTO 11 17)
set(CMAKE_AUTORCC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Keep MSVC import libraries and .exp files out of the runtime directory.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set("CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_BINARY_DIR}/lib/${config}")
endforeach()

# Platform policy:
#   - Windows and Linux: Qt 5.15 (the supplied desktop scripts use 5.15.2)
#   - Android and macOS: Qt 6
# AUTO follows that policy. Explicit overrides remain available for platform
# build scripts, but Qt 6 is intentionally rejected on Windows and Linux.
set(PEARE_QT_MAJOR "AUTO" CACHE STRING "Qt major version: AUTO, 5 or 6")
set_property(CACHE PEARE_QT_MAJOR PROPERTY STRINGS AUTO 5 6)

if(PEARE_QT_MAJOR STREQUAL "AUTO")
    if(ANDROID OR APPLE)
        set(PEARE_EFFECTIVE_QT_MAJOR 6)
    else()
        set(PEARE_EFFECTIVE_QT_MAJOR 5)
    endif()
elseif(PEARE_QT_MAJOR STREQUAL "5" OR PEARE_QT_MAJOR STREQUAL "6")
    set(PEARE_EFFECTIVE_QT_MAJOR ${PEARE_QT_MAJOR})
else()
    message(FATAL_ERROR "PEARE_QT_MAJOR must be AUTO, 5 or 6")
endif()

if(PEARE_EFFECTIVE_QT_MAJOR STREQUAL "6" AND NOT (ANDROID OR APPLE))
    message(FATAL_ERROR "Qt 6 is reserved for Android and macOS; Windows and Linux use Qt 5.15")
endif()
if(ANDROID AND NOT PEARE_EFFECTIVE_QT_MAJOR STREQUAL "6")
    message(FATAL_ERROR "Android builds require Qt 6")
endif()

if(PEARE_EFFECTIVE_QT_MAJOR STREQUAL "5" AND CMAKE_CROSSCOMPILING)
    set(PEARE_QT_HOST_PATH "" CACHE PATH "Host Qt tools used while cross-compiling Qt 5")
    if(NOT PEARE_QT_HOST_PATH)
        message(FATAL_ERROR "PEARE_QT_HOST_PATH is required for a Qt 5 cross-build")
    endif()

    foreach(qt_tool qmake moc rcc uic)
        set(qt_tool_path "${PEARE_QT_HOST_PATH}/bin/${qt_tool}")
        if(NOT EXISTS "${qt_tool_path}")
            message(FATAL_ERROR "Missing host Qt tool: ${qt_tool_path}")
        endif()
        if(NOT TARGET Qt5::${qt_tool})
            add_executable(Qt5::${qt_tool} IMPORTED GLOBAL)
            set_target_properties(Qt5::${qt_tool} PROPERTIES IMPORTED_LOCATION "${qt_tool_path}")
        endif()
    endforeach()
endif()

if(PEARE_EFFECTIVE_QT_MAJOR STREQUAL "6")
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Core5Compat)
    set(PEARE_QT_PACKAGE Qt6)
    set(QT_VERSION_MAJOR 6)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets)
    set(PEARE_QT_PACKAGE Qt5)
    set(QT_VERSION_MAJOR 5)
endif()

if(PEARE_CXX_STANDARD STREQUAL "AUTO")
    if(PEARE_QT_MAJOR STREQUAL "6")
        set(PEARE_EFFECTIVE_CXX_STANDARD 17)
    else()
        set(PEARE_EFFECTIVE_CXX_STANDARD 11)
    endif()
elseif(PEARE_CXX_STANDARD STREQUAL "11" OR PEARE_CXX_STANDARD STREQUAL "17")
    set(PEARE_EFFECTIVE_CXX_STANDARD ${PEARE_CXX_STANDARD})
else()
    message(FATAL_ERROR "Peare requires PEARE_CXX_STANDARD=AUTO, 11 or 17")
endif()
set(CMAKE_CXX_STANDARD ${PEARE_EFFECTIVE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


set(PEARE_QT_CORE ${PEARE_QT_PACKAGE}::Core)
set(PEARE_QT_GUI ${PEARE_QT_PACKAGE}::Gui)
set(PEARE_QT_WIDGETS ${PEARE_QT_PACKAGE}::Widgets)
# Decompression (deflate / zlib) is provided by the vendored miniz — a single
# self-contained, zlib-compatible C file — so Peare depends on neither an external
# nor a Qt-bundled zlib and stays portable (OS/2 included). It exposes the standard
# zlib names (z_stream, inflate, uncompress, ...), so callers just include <miniz.h>.
add_library(PeareMiniz STATIC third_party/miniz/miniz.c)
target_include_directories(PeareMiniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)
set_target_properties(PeareMiniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(PEARE_QT_ZLIB PeareMiniz)
if(QT_VERSION_MAJOR EQUAL 6)
    set(PEARE_QT_CORE5COMPAT Qt6::Core5Compat)
else()
    set(PEARE_QT_CORE5COMPAT)
endif()

message(STATUS "Peare Qt profile: Qt ${QT_VERSION_MAJOR}")
message(STATUS "Peare source baseline: C++11; compiler mode: C++${PEARE_EFFECTIVE_CXX_STANDARD}")

if(WIN32)
    message(STATUS "Peare targets: Peare.exe + PeareOpener.dll + PeareDecoder.dll")
elseif(APPLE)
    message(STATUS "Peare targets: Peare + libPeareOpener.dylib + libPeareDecoder.dylib")
else()
    message(STATUS "Peare targets: Peare + libPeareOpener + libPeareDecoder")
endif()

# Isolated upstream LZX engine.  Container-specific XEX framing remains outside
# this target.  It is compiled independently so licensing and dependencies stay
# explicit and the decoder can be adapted without coupling it to PeareOpener.
add_library(PeareLzxEngine OBJECT
    third_party/wimlib_lzx/src/lzx-common.c
    third_party/wimlib_lzx/src/lzx-decompress.c
    third_party/wimlib_lzx/src/decompress_common.c
    third_party/wimlib_lzx/src/aligned_alloc_compat.c
    third_party/wimlib_lzx/src/lzx-frontends.c
)
target_include_directories(PeareLzxEngine PUBLIC third_party/wimlib_lzx/include)
set_target_properties(PeareLzxEngine PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(PeareLzxEngine PRIVATE -std=gnu11)
endif()

add_library(PeareOpener SHARED
    src/opener/api/peare/peare_types.h
    src/opener/api/peare/peare_opener.h
    src/opener/api/Api.cpp
    src/opener/api/PeareResourceSnapshot.h
    src/opener/OpenerSession.h
    src/opener/OpenerSession.cpp
    src/opener/modules/Module.h
    src/opener/modules/Compat.h
    src/opener/modules/ModuleFactory.h
    src/opener/modules/ModuleFactory.cpp
    src/opener/modules/ModuleFormat.h
    src/opener/modules/ModuleFormat.cpp
    src/opener/modules/PeModule.h
    src/opener/modules/PeModuleBase.h
    src/opener/modules/PeModuleCommon.h
    src/opener/modules/PeModuleCommon.cpp
    src/opener/PeImageExport.h
    src/opener/PeImageExport.cpp
    src/opener/modules/PeFileModule.h
    src/opener/modules/PeFileModule.cpp
    src/opener/modules/PeImageModule.h
    src/opener/modules/PeImageModule.cpp
    src/opener/modules/XexModule.h
    src/opener/modules/XexModule.cpp
    src/opener/modules/Crypto.h
    src/opener/modules/Crypto.cpp
    third_party/tiny_aes/aes.h
    third_party/tiny_aes/aes.c
    src/opener/modules/XbeModule.h
    src/opener/modules/XbeModule.cpp
    src/opener/modules/XuizModule.h
    src/opener/modules/XuizModule.cpp
    src/opener/modules/LivePirsModule.h
    src/opener/modules/LivePirsModule.cpp
    src/opener/modules/ConModule.h
    src/opener/modules/ConModule.cpp
    src/opener/modules/Os2PackModule.h
    src/opener/modules/Os2PackModule.cpp
    src/opener/modules/SzddModule.h
    src/opener/modules/SzddModule.cpp
    src/opener/modules/SiemensImgModule.h
    src/opener/modules/SiemensImgModule.cpp
    src/opener/modules/SiemensFwfModule.h
    src/opener/modules/SiemensFwfModule.cpp
    src/opener/modules/EmbeddedZlibInflate.h
    src/opener/modules/SiemensImgParser.h
    src/opener/modules/SiemensFwfParser.h
    src/opener/modules/stfs/STFSCommon.h
    src/opener/modules/stfs/STFSCommon.cpp
    src/opener/modules/NeModule.h
    src/opener/modules/NeModule.cpp
    src/opener/modules/LxModule.h
    src/opener/modules/LxModule.cpp
    src/opener/modules/LeModule.h
    src/opener/modules/LeModule.cpp
    src/opener/modules/PeResources.h
    src/opener/modules/PeResources.cpp
    src/opener/fs/DiscStore.h
    src/opener/fs/BlockDecompressor.h
    src/opener/fs/DiscFileSystem.h
    src/opener/fs/Iso9660Reader.h
    src/opener/fs/Iso9660Reader.cpp
    src/opener/fs/BlockDecompressor.h
    src/opener/fs/XpressHuffman.cpp
    src/opener/fs/WimReader.h
    src/opener/fs/WimReader.cpp
    src/opener/fs/FatReader.h
    src/opener/fs/FatReader.cpp
    src/opener/fs/UdfReader.h
    src/opener/fs/UdfReader.cpp
    src/opener/fs/ExFatReader.h
    src/opener/fs/ExFatReader.cpp
    src/opener/fs/VmdkDisk.h
    src/opener/fs/VmdkDisk.cpp
    src/opener/fs/VhdDisk.h
    src/opener/fs/VhdDisk.cpp
    src/opener/fs/VdiDisk.h
    src/opener/fs/VdiDisk.cpp
    src/opener/fs/VhdxDisk.h
    src/opener/fs/VhdxDisk.cpp
    src/opener/fs/PartitionTable.h
    src/opener/fs/PartitionTable.cpp
    src/opener/fs/LinuxRaid.h
    src/opener/fs/LinuxRaid.cpp
    src/opener/fs/DynamicDisk.h
    src/opener/fs/DynamicDisk.cpp
    src/opener/fs/ExtReader.h
    src/opener/fs/ExtReader.cpp
    src/opener/fs/NtfsReader.h
    src/opener/fs/NtfsReader.cpp
    src/opener/fs/XfsReader.h
    src/opener/fs/XfsReader.cpp
    src/opener/fs/SquashFsReader.h
    src/opener/fs/SquashFsReader.cpp
    src/opener/fs/HfsPlusReader.h
    src/opener/fs/HfsPlusReader.cpp
    src/opener/fs/DmgDisk.h
    src/opener/fs/DmgDisk.cpp
    src/opener/fs/BtrfsReader.h
    src/opener/fs/BtrfsReader.cpp
    src/opener/fs/RegistryHiveReader.h
    src/opener/fs/RegistryHiveReader.cpp
    src/opener/modules/FsLevel.h
    src/opener/modules/IsoModule.h
    src/opener/modules/IsoModule.cpp
    src/opener/modules/WimModule.h
    src/opener/modules/WimModule.cpp
    src/opener/modules/FatModule.h
    src/opener/modules/FatModule.cpp
    src/opener/modules/UdfModule.h
    src/opener/modules/UdfModule.cpp
    src/opener/modules/ExFatModule.h
    src/opener/modules/ExFatModule.cpp
    src/opener/modules/VmdkModule.h
    src/opener/modules/VmdkModule.cpp
    src/opener/modules/VhdModule.h
    src/opener/modules/VhdModule.cpp
    src/opener/modules/VdiModule.h
    src/opener/modules/VdiModule.cpp
    src/opener/modules/VhdxModule.h
    src/opener/modules/VhdxModule.cpp
    src/opener/modules/SdiModule.h
    src/opener/modules/SdiModule.cpp
    src/opener/modules/XvaModule.h
    src/opener/modules/XvaModule.cpp
    src/opener/modules/SwapModule.h
    src/opener/modules/SwapModule.cpp
    src/opener/modules/LvmModule.h
    src/opener/modules/LvmModule.cpp
    src/opener/modules/LinuxRaidModule.h
    src/opener/modules/LinuxRaidModule.cpp
    src/opener/modules/DynamicDiskModule.h
    src/opener/modules/DynamicDiskModule.cpp
    src/opener/modules/ExtModule.h
    src/opener/modules/ExtModule.cpp
    src/opener/modules/NtfsModule.h
    src/opener/modules/NtfsModule.cpp
    src/opener/modules/XfsModule.h
    src/opener/modules/XfsModule.cpp
    src/opener/modules/SquashFsModule.h
    src/opener/modules/SquashFsModule.cpp
    src/opener/modules/HfsPlusModule.h
    src/opener/modules/HfsPlusModule.cpp
    src/opener/modules/DmgModule.h
    src/opener/modules/DmgModule.cpp
    src/opener/modules/BtrfsModule.h
    src/opener/modules/BtrfsModule.cpp
    src/opener/modules/RegistryModule.h
    src/opener/modules/RegistryModule.cpp
    src/opener/modules/BootConfigModule.h
    src/opener/modules/BootConfigModule.cpp
    src/opener/modules/CabModule.h
    src/opener/modules/CabModule.cpp
    src/opener/modules/RawDiskModule.h
    src/opener/modules/RawDiskModule.cpp
    src/opener/modules/ZipModule.h
    src/opener/modules/ZipModule.cpp
    src/opener/modules/TarModule.h
    src/opener/modules/TarModule.cpp
)
target_compile_definitions(PeareOpener PRIVATE PEARE_OPENER_BUILD)
target_include_directories(PeareOpener
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/opener/api>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(PeareOpener PRIVATE ${PEARE_QT_CORE} ${PEARE_QT_ZLIB} PeareLzxEngine)
target_include_directories(PeareOpener PRIVATE third_party/wimlib_lzx/include)
set_target_properties(PeareOpener PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME PeareOpener
)

add_library(PeareDecoder SHARED
    src/decoder/api/peare/peare.h
    src/opener/api/peare/peare_types.h
    src/decoder/api/peare/peare_decoder.h
    src/decoder/api/Api.cpp
    src/decoder/api/PeareResourceBridge.h
    src/decoder/Optional.h
    src/decoder/DecoderTypes.h
    src/decoder/DecoderRoute.h
    src/decoder/DecoderRoute.cpp
    src/decoder/ResourceDecoder.h
    src/decoder/ResourceDecoder.cpp
    src/decoder/resources/ResourceResolver.h
    src/decoder/resources/ModuleResources.h
    src/decoder/resources/ModuleResources.cpp
    src/decoder/resources/RawDetect.h
    src/decoder/resources/RawDetect.cpp
    src/decoder/ResourceExport.h
    src/decoder/ResourceExport.cpp
    src/decoder/resources/XBE_LOGO_RLE/XBE_LOGO_RLE.h
    src/decoder/resources/XBE_LOGO_RLE/XBE_LOGO_RLE.cpp
    src/decoder/resources/FMIM/FMIM.h
    src/decoder/resources/FMIM/FMIM.cpp
    src/decoder/resources/WSZ/WszDecoder.h
    src/decoder/resources/WSZ/WszDecoder.cpp
    src/decoder/resources/RT_ICON/Win12MonochromeResource.h
    src/decoder/resources/RT_ICON/Win12MonochromeResource.cpp
    src/decoder/resources/RT_ICON/RT_ICON.h
    src/decoder/resources/RT_ICON/RT_ICON.cpp
    src/decoder/resources/RT_GROUP_ICON/RT_GROUP_ICON.h
    src/decoder/resources/RT_GROUP_ICON/RT_GROUP_ICON.cpp
    src/decoder/resources/RT_CURSOR/RT_CURSOR.h
    src/decoder/resources/RT_CURSOR/RT_CURSOR.cpp
    src/decoder/resources/RT_GROUP_CURSOR/RT_GROUP_CURSOR.h
    src/decoder/resources/RT_GROUP_CURSOR/RT_GROUP_CURSOR.cpp
    src/decoder/resources/RT_BITMAP/RT_BITMAP.h
    src/decoder/resources/RT_BITMAP/RT_BITMAP.cpp
    src/decoder/resources/RT_POINTER/RT_POINTER.h
    src/decoder/resources/RT_POINTER/RT_POINTER.cpp
    src/decoder/resources/RT_STRING/RT_STRING.h
    src/decoder/resources/RT_STRING/RT_STRING.cpp
    src/decoder/resources/RT_DLGINCLUDE/RT_DLGINCLUDE.h
    src/decoder/resources/RT_DLGINCLUDE/RT_DLGINCLUDE.cpp
    src/decoder/resources/RT_MESSAGE/RT_MESSAGE.h
    src/decoder/resources/RT_MESSAGE/RT_MESSAGE.cpp
    src/decoder/resources/RT_VERSION/RT_VERSION.h
    src/decoder/resources/RT_VERSION/RT_VERSION.cpp
    src/decoder/resources/RT_ACCELERATOR/RT_ACCELERATOR.h
    src/decoder/resources/RT_ACCELERATOR/RT_ACCELERATOR.cpp
    src/decoder/resources/RT_ACCELTABLE/RT_ACCELTABLE.h
    src/decoder/resources/RT_ACCELTABLE/RT_ACCELTABLE.cpp
    src/decoder/resources/RT_FONT/RT_FONT.h
    src/decoder/resources/RT_FONT/RT_FONT.cpp
    src/decoder/resources/RT_FONT/OS2_RT_FONT.h
    src/decoder/resources/RT_FONT/OS2_RT_FONT.cpp
    src/decoder/resources/RT_FONTDIR/RT_FONTDIR.h
    src/decoder/resources/RT_FONTDIR/RT_FONTDIR.cpp
    src/decoder/resources/RT_DISPLAYINFO/RT_DISPLAYINFO.h
    src/decoder/resources/RT_DISPLAYINFO/RT_DISPLAYINFO.cpp
    src/decoder/resources/RT_HELPTABLE/RT_HELPTABLE.h
    src/decoder/resources/RT_HELPTABLE/RT_HELPTABLE.cpp
    src/decoder/resources/RT_HELPSUBTABLE/RT_HELPSUBTABLE.h
    src/decoder/resources/RT_HELPSUBTABLE/RT_HELPSUBTABLE.cpp
    src/decoder/resources/RT_NAMETABLE/RT_NAMETABLE.h
    src/decoder/resources/RT_NAMETABLE/RT_NAMETABLE.cpp
    src/decoder/resources/RT_MENU/RT_MENU.h
    src/decoder/resources/RT_MENU/RT_MENU.cpp
    src/decoder/resources/RT_MENU/OS2_RT_MENU.h
    src/decoder/resources/RT_MENU/OS2_RT_MENU.cpp
    src/decoder/resources/RT_DIALOG/RT_DIALOG.h
    src/decoder/resources/RT_DIALOG/RT_DIALOG.cpp
    src/decoder/resources/RT_DIALOG/OS2_RT_DIALOG.h
    src/decoder/resources/RT_DIALOG/OS2_RT_DIALOG.cpp
)
target_compile_definitions(PeareDecoder PRIVATE PEARE_DECODER_BUILD)
target_include_directories(PeareDecoder
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/opener/api>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/api>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(PeareDecoder
    PRIVATE ${PEARE_QT_CORE} ${PEARE_QT_GUI} ${PEARE_QT_CORE5COMPAT}
)
set_target_properties(PeareDecoder PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME PeareDecoder
)

set(PEARE_APP_SOURCES
    src/app/main.cpp
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/PeareApi.h
    src/app/PeareApi.cpp
    src/app/IconFromExt.h
    src/app/IconFromExt.cpp
    src/app/SettingsIcons.h
    src/app/SettingsIcons.cpp
    resources/peare.qrc
)

if(ANDROID AND QT_VERSION_MAJOR GREATER_EQUAL 6)
    # qt_add_executable creates the Android shared-library application target
    # and the deployment metadata used by androiddeployqt. Qt Widgets remains
    # the GUI technology; no QML conversion is required.
    qt_add_executable(Peare MANUAL_FINALIZATION ${PEARE_APP_SOURCES})
else()
    add_executable(Peare ${PEARE_APP_SOURCES})
endif()

if(WIN32)
    target_sources(Peare PRIVATE resources/peare.rc)
endif()

target_include_directories(Peare PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/api
)
target_link_libraries(Peare PRIVATE PeareOpener PeareDecoder ${PEARE_QT_WIDGETS})

if(ANDROID AND QT_VERSION_MAJOR GREATER_EQUAL 6)
    set_property(TARGET Peare PROPERTY QT_ANDROID_PACKAGE_NAME "org.peare.app")
    qt_finalize_executable(Peare)
endif()

install(TARGETS PeareOpener PeareDecoder
    EXPORT PeareTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    src/decoder/api/peare/peare.h
    src/opener/api/peare/peare_types.h
    src/opener/api/peare/peare_opener.h
    src/decoder/api/peare/peare_decoder.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/peare
)

configure_package_config_file(
    cmake/PeareConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/PeareConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peare
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/PeareConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(EXPORT PeareTargets
    FILE PeareTargets.cmake
    NAMESPACE Peare::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peare
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/PeareConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/PeareConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peare
)
