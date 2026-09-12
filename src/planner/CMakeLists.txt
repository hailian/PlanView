# LogicPlanner — 逻辑规划软件（设计器）
add_executable(LogicPlanner
    main.cpp
    PlannerApp.cpp
    PlannerContext.cpp
    Document.cpp
    panels/Palette.cpp
    panels/Pages.cpp
    panels/Canvas.cpp
    panels/Inspector.cpp
    panels/Associations.cpp
    panels/Tags.cpp
    panels/Validation.cpp
)
target_link_libraries(LogicPlanner PRIVATE softg_base)
if(MSVC)
    target_compile_options(LogicPlanner PRIVATE /W3)
endif()
