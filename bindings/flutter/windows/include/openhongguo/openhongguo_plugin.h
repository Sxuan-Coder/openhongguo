// openhongguo Flutter 插件的 Windows 端（W2 桌面移植，路线 A）。
//
// 与 Android 端 OpenhongguoPlugin.kt 1:1 对应：同一个 MethodChannel
// 'dev.openhongguo/flutter'、同样的 createClient/call/releaseClient 协议，
// Dart 层（lib/src/client.dart）零改动。
//
// 区别在于原生实现：Kotlin 走 gomobile 的 core.jar + libgojni.so，这里
// LoadLibrary 加载随包分发的 openhongguo.dll（cgo -buildmode=c-shared
// 产物，再生方法见 windows/BUILD_DLL.md），调用其 C ABI（OHG_* 导出）。
//
// 本文件是 Flutter 3.47 Windows 插件注册协议的公共面：生成的
// generated_plugin_registrant.cc 会 include 本头并调用导出函数
// OpenhongguoPluginRegisterWithRegistrar（函数名 = pubspec 的
// pluginClass + "RegisterWithRegistrar"，必须位于全局命名空间）。
// 实现细节（线程模型、客户端生命周期）见 openhongguo_plugin.cpp。

#ifndef FLUTTER_PLUGIN_OPENHONGGUO_PLUGIN_H_
#define FLUTTER_PLUGIN_OPENHONGGUO_PLUGIN_H_

#include <windows.h>

#include <flutter_plugin_registrar.h>

#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FLUTTER_PLUGIN_EXPORT __declspec(dllimport)
#endif

#if defined(__cplusplus)
extern "C" {
#endif

FLUTTER_PLUGIN_EXPORT void OpenhongguoPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_PLUGIN_OPENHONGGUO_PLUGIN_H_
