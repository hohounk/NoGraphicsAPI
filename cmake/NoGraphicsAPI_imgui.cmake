# The fork's own tests and examples fetch imgui. A consumer provides its own imgui target.
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG b916186e66aa3c02b72c6ca17455516512ed8049 # v1.93.0-docking
)
FetchContent_MakeAvailable(imgui)

add_library(${NOGRAPHICSAPI_IMGUI_TARGET} STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
target_include_directories(${NOGRAPHICSAPI_IMGUI_TARGET} PUBLIC ${imgui_SOURCE_DIR})
target_compile_features(${NOGRAPHICSAPI_IMGUI_TARGET} PUBLIC cxx_std_20)
