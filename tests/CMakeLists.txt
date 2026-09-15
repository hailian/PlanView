# 单测：自研极简框架 PvTest（零依赖）
add_executable(pv_tests
    test_Smoke.cpp
    test_ModelJson.cpp
    test_TcpData.cpp
    test_RuntimeEngine.cpp
    test_PacketTools.cpp
    test_FrameSource.cpp
    test_PcapListen.cpp
    test_UsbLink.cpp
    test_VisaLink.cpp
    test_Align.cpp
    test_AiProto.cpp
    test_Perf.cpp
    test_Components.cpp
    test_PollWorker.cpp
    # PollWorker 本体随测试目标编译（仅依赖 pv_base，不拉入 ViewerApp/ImGui）
    ${CMAKE_SOURCE_DIR}/src/viewer/PollWorker.cpp
)
target_link_libraries(pv_tests PRIVATE pv_base)
target_include_directories(pv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME pv_tests COMMAND pv_tests)
