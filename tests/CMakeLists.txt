# 单测：自研极简框架 SoftgTest（零依赖）
add_executable(softg_tests
    test_Smoke.cpp
    test_ModelJson.cpp
    test_TcpData.cpp
    test_RuntimeEngine.cpp
    test_PacketTools.cpp
    test_FrameSource.cpp
    test_Align.cpp
    test_AiProto.cpp
)
target_link_libraries(softg_tests PRIVATE softg_base)
target_include_directories(softg_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME softg_tests COMMAND softg_tests)
