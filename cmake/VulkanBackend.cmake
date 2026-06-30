# VulkanBackend.cmake
#
# Wires the Vulkan 1.3 backend (rvk) into the raylib build. Activated by
# -DGRAPHICS_API_VULKAN=ON (desktop GLFW platform only). Pulls the required
# dependencies via FetchContent per the architecture directive:
#   vk-bootstrap, VMA (VulkanMemoryAllocator), shaderc are fetched/located;
#   Vulkan-Headers come from the Vulkan SDK (find_package) or are fetched.
#
# This module is included from src/CMakeLists.txt only when GRAPHICS_API_VULKAN is ON.

include(FetchContent)

message(STATUS "rvk: configuring Vulkan 1.3 backend")

# --- Vulkan (loader + headers) -------------------------------------------------
# Prefer the system/SDK Vulkan package; fall back to fetching Vulkan-Headers.
find_package(Vulkan QUIET)
if (NOT Vulkan_FOUND)
    message(STATUS "rvk: Vulkan SDK not found via find_package, fetching Vulkan-Headers")
    FetchContent_Declare(VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG        v1.3.275)
    FetchContent_MakeAvailable(VulkanHeaders)
endif ()

# --- vk-bootstrap (instance/device/swapchain helper) ---------------------------
FetchContent_Declare(fetch_vk_bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG        v1.3.275)
FetchContent_MakeAvailable(fetch_vk_bootstrap)

# --- VMA (Vulkan Memory Allocator, header-only) --------------------------------
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# --- shaderc (runtime GLSL -> SPIR-V) ------------------------------------------
# shaderc ships with the Vulkan SDK as 'shaderc_combined'/'shaderc_shared'.
# Building shaderc from source via FetchContent is heavy; prefer the SDK library.
find_library(SHADERC_LIB NAMES shaderc_combined shaderc_shared shaderc
             HINTS ENV VULKAN_SDK PATH_SUFFIXES lib)
if (NOT SHADERC_LIB)
    message(WARNING "rvk: shaderc not found. Install the Vulkan SDK or provide SHADERC_LIB. "
                    "Runtime GLSL compilation (rvkLoadShader) will be unavailable.")
endif ()

# --- rvk sources ---------------------------------------------------------------
set(RVK_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/rvk_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rvk_memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rvk_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rvk_batcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/rvk_compute.cpp
)

# Expose to the parent scope (src/CMakeLists.txt appends these to raylib_sources)
set(RVK_SOURCES ${RVK_SOURCES} PARENT_SCOPE)

# Function applied to the raylib target by src/CMakeLists.txt
function(rvk_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/include            # rvk.h
        ${CMAKE_CURRENT_SOURCE_DIR}            # rvk_internal.hpp
    )
    target_compile_definitions(${target} PRIVATE
        GRAPHICS_API_VULKAN
        VK_NO_PROTOTYPES=0
        RVK_ENABLE_VALIDATION=$<IF:$<CONFIG:Debug>,1,0>
    )
    target_link_libraries(${target} PRIVATE
        vk-bootstrap::vk-bootstrap
        GPUOpen::VulkanMemoryAllocator
    )
    if (Vulkan_FOUND)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
    else ()
        target_link_libraries(${target} PRIVATE Vulkan::Headers vulkan)
    endif ()
    if (SHADERC_LIB)
        target_link_libraries(${target} PRIVATE ${SHADERC_LIB})
        target_compile_definitions(${target} PRIVATE RVK_HAVE_SHADERC=1)
    endif ()
endfunction()
