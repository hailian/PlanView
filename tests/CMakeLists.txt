# 单测：自研极简框架 PvTest（零依赖）
add_executable(pv_tests
    test_Smoke.cpp
    test_ModelJson.cpp
    test_TcpData.cpp
    test_RuntimeEngine.cpp
    test_PacketTools.cpp
    test_FrameSource.cpp
    test_PcapListen.cpp
    test_Align.cpp
    test_AiProto.cpp
)
target_link_libraries(pv_tests PRIVATE pv_base)
target_include_directories(pv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pv_tests COMMAND pv_tests)
