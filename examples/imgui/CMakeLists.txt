if(NOT imgui_SOURCE_DIR)
    message(FATAL_ERROR "example_imgui compiles backends/imgui_impl_sdl3.cpp from imgui_SOURCE_DIR, which only the fetched imgui target sets")
endif()
find_package(SDL3 REQUIRED CONFIG)

add_executable(example_imgui
    imgui_example.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
)
target_include_directories(example_imgui PRIVATE ${imgui_SOURCE_DIR}/backends)
NoGraphicsAPI_disable_exceptions(example_imgui)
target_link_libraries(example_imgui PRIVATE NoGraphicsAPI::ImGuiBackend SDL3::SDL3)
