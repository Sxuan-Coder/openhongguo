# openhongguo 插件的 Windows 端构建。
#
# 产物两个：
#   openhongguo_plugin.dll  — 本目录 openhongguo_plugin.cpp 编出的
#                             MethodChannel 桥（C++/MSVC）；
#   openhongguo.dll         — go cgo -buildmode=c-shared 产物（随源入库，
#                             再生方法见 BUILD_DLL.md），经 bundled_libraries
#                             机制与插件 DLL 一起拷到应用 exe 旁。
cmake_minimum_required(VERSION 3.14)
set(PROJECT_NAME "openhongguo")
project(${PROJECT_NAME} LANGUAGES CXX)

# 插件目标带 _plugin 后缀，避免与随包分发的 openhongguo.dll 撞名。
set(PLUGIN_NAME "openhongguo_plugin")

add_library(${PLUGIN_NAME} SHARED
  "openhongguo_plugin.cpp"
)

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
# 源文件为 UTF-8（含中文注释），MSVC 默认按系统代码页（936）解读会报
# C4819（/WX 下即错），显式声明 /utf-8。
target_compile_options(${PLUGIN_NAME} PRIVATE "/utf-8")
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
# version：读宿主 exe 的 ProductName/CompanyName（默认档案目录）；
# shell32 + ole32：SHGetKnownFolderPath / CoTaskMemFree。
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin
  version shell32 ole32)

# 随包分发（flutter_wrapper 的 bundled_libraries 约定：拷到应用 exe 旁）。
set(openhongguo_bundled_libraries
  "${CMAKE_CURRENT_SOURCE_DIR}/openhongguo.dll"
  PARENT_SCOPE
)
