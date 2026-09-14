# PageViewer — 页面展示软件（运行器）
add_executable(PageViewer
    main.cpp
    app.rc
    ViewerApp.cpp
    PollWorker.cpp
)
target_link_libraries(PageViewer PRIVATE softg_base)
if(MSVC)
    target_compile_options(PageViewer PRIVATE /W3)
endif()
