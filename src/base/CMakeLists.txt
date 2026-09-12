# softg_base — 逻辑规划软件与页面展示软件共用的静态库
add_library(softg_base STATIC
    appshell/AppShell.cpp
    appshell/FileDialog.cpp
    appshell/FontSetup.cpp
    log/Log.cpp
    model/ComponentRegistry.cpp
    model/Tag.cpp
    model/TagDatabase.cpp
    render/ComponentRenderer.cpp
    render/TextureCache.cpp
    render/widgets/Widgets.cpp
    serialize/JsonHelpers.cpp
    serialize/ProjectJson.cpp
    data/DataSourceManager.cpp
    data/tcp/SoftgProtocol.cpp
    data/tcp/TcpDataSource.cpp
    runtime/RuntimeEngine.cpp
)

target_include_directories(softg_base PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(softg_base PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_link_libraries(softg_base PUBLIC
    imgui
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    ws2_32
)

if(MSVC)
    target_compile_options(softg_base PRIVATE /W3)
endif()
