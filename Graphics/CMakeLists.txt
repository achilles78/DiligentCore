cmake_minimum_required (VERSION 3.6)

set(DLL_REL_SUFFIX _${ARCH}r)
set(DLL_DBG_SUFFIX _${ARCH}d)

add_library(GraphicsEngineInterface INTERFACE)
target_include_directories(GraphicsEngineInterface
INTERFACE
    GraphicsEngine/interface
)
target_link_libraries(GraphicsEngineInterface
INTERFACE
    Primitives
)

add_subdirectory(GraphicsAccessories)
add_subdirectory(GraphicsEngine)

if(D3D11_SUPPORTED OR D3D12_SUPPORTED)
    add_subdirectory(GraphicsEngineD3DBase)
endif()

if(D3D11_SUPPORTED)
    add_subdirectory(GraphicsEngineD3D11)
endif()

if(D3D12_SUPPORTED)
    add_subdirectory(GraphicsEngineD3D12)
endif()

if(GL_SUPPORTED OR GLES_SUPPORTED OR VULKAN_SUPPORTED)
    add_subdirectory(GLSLTools)
endif()

if(VULKAN_SUPPORTED)
    add_subdirectory(GraphicsEngineVulkan)
endif()

if(GL_SUPPORTED OR GLES_SUPPORTED OR VULKAN_SUPPORTED)
    add_subdirectory(HLSL2GLSLConverterLib)
    add_subdirectory(GraphicsEngineOpenGL)
endif()

add_subdirectory(GraphicsTools)