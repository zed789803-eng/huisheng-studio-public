cmake_minimum_required(VERSION 3.22)
project(HuishengStudioAudio VERSION 1.0.12 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures" FORCE)

# JUCE is pinned to 8.0.15. A checkout at third_party/JUCE is used when present
# (offline builds, and what developers here have on disk); otherwise CMake
# fetches the same tagged tarball and verifies its SHA-256, so a fresh clone
# builds without a manual submodule step.
set(HS_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE")
if(EXISTS "${HS_JUCE_DIR}/CMakeLists.txt")
    add_subdirectory(${HS_JUCE_DIR} ${CMAKE_BINARY_DIR}/third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        URL      https://codeload.github.com/juce-framework/JUCE/tar.gz/refs/tags/8.0.15
        URL_HASH SHA256=0eafd736286c6e7bb6958d79fce18d42cb654b8e231890d327b7cece379bd156
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

option(HS_SKIP_PLUGIN_COPY "Do not install VST3 bundles into ~/Library" OFF)

set(HS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------------
# Shared audio primitives. Compiled into every target: the Agent, both plug-ins
# and the driver's loopback does not need them, but the HAL build links the ring
# and resampler so behaviour matches the plug-ins exactly.
# -----------------------------------------------------------------------------
# Logo, reused by the plug-in editors and the Agent icon.
juce_add_binary_data(HsAssets
    HEADER_NAME "BinaryData.h"
    NAMESPACE hs_assets
    SOURCES ${HS_ROOT}/ui/logo.png
)

add_library(hs_audio STATIC
    shared/RingBuffer.cpp
    shared/Resampler.cpp
    shared/DriftBridge.cpp
    shared/License.cpp
)
target_include_directories(hs_audio PUBLIC ${HS_ROOT}/shared)
target_compile_features(hs_audio PUBLIC cxx_std_20)

# -----------------------------------------------------------------------------
# Background Agent. Owns TCC and the tap; feeds the plug-ins over shared memory.
# -----------------------------------------------------------------------------
add_executable(HuishengStudioAgent MACOSX_BUNDLE
    agent/AgentMain.mm
    agent/Core.mm
    agent/PermissionGuide.mm
    hub/AudioDevices.mm
    hub/ProcessList.mm
    hub/ProcessTap.mm
    hub/RingPlayer.mm
    hub/RingRecorder.mm
    hub/Permissions.mm
)

set(HS_OBJC_SOURCES
    agent/AgentMain.mm
    agent/Core.mm
    agent/PermissionGuide.mm
    hub/AudioDevices.mm
    hub/ProcessList.mm
    hub/ProcessTap.mm
    hub/RingPlayer.mm
    hub/RingRecorder.mm
    hub/Permissions.mm
)
set_source_files_properties(${HS_OBJC_SOURCES} PROPERTIES COMPILE_FLAGS "-fobjc-arc")

target_include_directories(HuishengStudioAgent PRIVATE ${HS_ROOT}/hub)
target_link_libraries(HuishengStudioAgent PRIVATE
    hs_audio
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework AVFoundation"
    "-framework AppKit"
    "-framework Foundation"
)

set_target_properties(HuishengStudioAgent PROPERTIES
    OUTPUT_NAME "HuishengStudio Agent"
    MACOSX_BUNDLE_BUNDLE_NAME "绘声Studio"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.huishengstudio.agent"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.12"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.12"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/agent"
)

# -----------------------------------------------------------------------------
# Plug-ins. VST3 serves Cubase / Nuendo / Studio One / Reaper / Bitwig; AU (v2)
# serves Logic Pro / GarageBand / MainStage, which have no VST support at all.
# Both wrappers are built from the same sources and share the manufacturer and
# product codes, so one plug-in keeps one identity across formats.
# -----------------------------------------------------------------------------
set(HS_PLUGIN_COMMON
    plugins/AgentLauncher.mm
    hub/ProcessList.mm
    hub/AudioDevices.mm
)

set(HS_PLUGIN_LIBS
    hs_audio
    juce::juce_audio_processors
    juce::juce_gui_extra
    "-framework CoreAudio"
    "-framework AppKit"
    "-framework Foundation"
)

juce_add_plugin(HuishengStudioCapture
    PRODUCT_NAME "HsAudio Capture"
    COMPANY_NAME "HsAudio"
    BUNDLE_ID "com.huishengstudio.capture"
    PLUGIN_MANUFACTURER_CODE Huis
    PLUGIN_CODE Cap1
    FORMATS VST3 AU
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Fx
)

juce_generate_juce_header(HuishengStudioCapture)

target_sources(HuishengStudioCapture PRIVATE
    plugins/Capture/CaptureProcessor.cpp
    plugins/Capture/CaptureEditor.cpp
    ${HS_PLUGIN_COMMON}
)

target_include_directories(HuishengStudioCapture PRIVATE
    ${HS_ROOT}/ui
    ${HS_ROOT}/plugins
    ${HS_ROOT}/plugins/Capture
    ${HS_ROOT}/hub
)

target_link_libraries(HuishengStudioCapture PRIVATE ${HS_PLUGIN_LIBS} HsAssets)
target_compile_definitions(HuishengStudioCapture PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

juce_add_plugin(HuishengStudioReturn
    PRODUCT_NAME "HsAudio Return"
    COMPANY_NAME "HsAudio"
    BUNDLE_ID "com.huishengstudio.return"
    PLUGIN_MANUFACTURER_CODE Huis
    PLUGIN_CODE Ret1
    FORMATS VST3 AU
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES Fx
)

juce_generate_juce_header(HuishengStudioReturn)

target_sources(HuishengStudioReturn PRIVATE
    plugins/Return/ReturnProcessor.cpp
    plugins/Return/ReturnEditor.cpp
    ${HS_PLUGIN_COMMON}
)

target_include_directories(HuishengStudioReturn PRIVATE
    ${HS_ROOT}/ui
    ${HS_ROOT}/plugins
    ${HS_ROOT}/plugins/Return
    ${HS_ROOT}/hub
)

target_link_libraries(HuishengStudioReturn PRIVATE ${HS_PLUGIN_LIBS} HsAssets)
target_compile_definitions(HuishengStudioReturn PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

if(HS_SKIP_PLUGIN_COPY)
    set_target_properties(HuishengStudioCapture HuishengStudioReturn
        PROPERTIES JUCE_COPY_PLUGIN_AFTER_BUILD FALSE)
endif()

# -----------------------------------------------------------------------------
# Diagnostics.
# -----------------------------------------------------------------------------
add_executable(hsmon tools/hsmon.cpp)
target_link_libraries(hsmon PRIVATE hs_audio)

add_executable(hsdoctor tools/doctor.cpp)
target_link_libraries(hsdoctor PRIVATE "-framework CoreAudio" "-framework CoreFoundation")

# Development-only: pretends to be the Return plug-in so the Agent's heartbeat
# handling can be checked without a DAW.
add_executable(hsreturnsim tools/return_sim.cpp)
target_include_directories(hsreturnsim PRIVATE ${HS_ROOT}/shared)
target_compile_features(hsreturnsim PRIVATE cxx_std_20)

# Registration code generator. Same secret and same encoding as the app, so a
# code printed here activates the build shipped from this repository.
add_executable(hskeygen tools/hskeygen.cpp)
target_link_libraries(hskeygen PRIVATE hs_audio)

# -----------------------------------------------------------------------------
# Development-only offscreen renderer for the two editors.
# -----------------------------------------------------------------------------
juce_add_gui_app(HuishengStudioPreview
    PRODUCT_NAME "HuishengStudio Preview"
    COMPANY_NAME "HuishengStudio"
    BUNDLE_ID "com.huishengstudio.preview"
    VERSION "1.0.12"
)

juce_generate_juce_header(HuishengStudioPreview)

target_sources(HuishengStudioPreview PRIVATE
    tools/preview/Preview.cpp
    plugins/Capture/CaptureProcessor.cpp
    plugins/Capture/CaptureEditor.cpp
    plugins/Return/ReturnProcessor.cpp
    plugins/Return/ReturnEditor.cpp
    ${HS_PLUGIN_COMMON}
)

target_include_directories(HuishengStudioPreview PRIVATE
    ${HS_ROOT}/ui
    ${HS_ROOT}/plugins
    ${HS_ROOT}/plugins/Capture
    ${HS_ROOT}/plugins/Return
    ${HS_ROOT}/hub
)

target_link_libraries(HuishengStudioPreview PRIVATE
    hs_audio
    HsAssets
    juce::juce_audio_processors
    juce::juce_gui_extra
    "-framework CoreAudio"
    "-framework AppKit"
    "-framework Foundation"
)

target_compile_definitions(HuishengStudioPreview PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 HS_PREVIEW=1)

# -----------------------------------------------------------------------------
# Registration code generator with a window. This one goes to the publisher who
# sells the tool, not to end users: it carries the product key.
# -----------------------------------------------------------------------------
juce_add_gui_app(HuishengStudioKeygen
    PRODUCT_NAME "HuishengStudio Keygen"
    COMPANY_NAME "HuishengStudio"
    BUNDLE_ID "com.huishengstudio.keygen"
    VERSION "1.0.12"
)

juce_generate_juce_header(HuishengStudioKeygen)

target_sources(HuishengStudioKeygen PRIVATE tools/keygen/KeygenApp.cpp)

target_include_directories(HuishengStudioKeygen PRIVATE ${HS_ROOT}/ui)

target_link_libraries(HuishengStudioKeygen PRIVATE
    hs_audio
    HsAssets
    juce::juce_gui_extra
    "-framework AppKit"
    "-framework Foundation"
)

target_compile_definitions(HuishengStudioKeygen PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 HS_KEYGEN=1)
