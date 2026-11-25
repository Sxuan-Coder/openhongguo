// openhongguo_plugin.cpp — MethodChannel 'dev.openhongguo/flutter' 的
// Windows 实现（W2 桌面移植，路线 A：pub 缓存包 fork + cgo DLL 桥接）。
//
// channel 方法 → openhongguo.dll C ABI 映射（对照 Android Kotlin 端
// OpenhongguoPlugin.kt 与 windows-poc RESULTS.md §4 蓝图）：
//
// | MethodChannel 'dev.openhongguo/flutter' | C ABI                    | 线程      |
// |------------------------------------------|--------------------------|-----------|
// | createClient {profilePath,config,log}    | OHGCreateClient          | platform  |
// | call {client,method,args}                | OHGCall                  | worker    |
// | releaseClient {client}                   | OHGReleaseClient         | platform  |
//
// 协议与 Kotlin 端一致：三个方法的返回值都是 JSON 字符串（{"id":n} /
// {"result":…} / {"error":{…}}），原样回传给 Dart 由 client.dart 解码
// ——本端唯一的解析点是从 OHGCreateClient 的 {"id":N} 里取 N 做客户端
// 生命周期记账（DLL 内部自增的编号，与 Kotlin 端 nextId.incrementAndGet
// 同一角色）。
//
// 线程模型：
// - createClient/releaseClient 不发网络请求，platform 线程直接处理；
// - OHGCall 同步阻塞，内部覆盖 retryAttempts=3 + deviceResets=2 的最坏
//   情况（约 5 分钟），必须丢到 C++ worker 线程；worker 是**固定大小线程
//   池**（见 WorkerPool），多个调用并行执行，与 Android 端每个调用一个
//   goroutine 的并发度一致。完成后从 worker 线程
//   调 result->Success()：引擎 C++ 包装的应答 lambda 自带 messenger 锁
//   与可用性检查（cpp_client_wrapper/core_implementations.cc 明确注释
//   "This lambda can be called on any thread."），等价于 Kotlin 端
//   client.start { main.post { result.success(out) } } 的回投语义，
//   且不占用 platform 线程。
//
// 客户端生命周期（release 先排空）：
// - 每个 client 一个 ClientEntry，带在途计数 in_flight；
// - release：先从注册表摘除（后续 call 立刻报 client was released），
//   若尚有在途 OHGCall，则把 release 的应答挂到 entry 上，由最后一个
//   在途调用返回后补调 OHGReleaseClient 并代答——Dart 侧
//   await release() 落地即代表该客户端再无任何在途结果，且全程不阻塞
//   platform 线程。
//
// 设备档案默认位置：%LOCALAPPDATA%\{CompanyName}\{ProductName}\openhongguo\
// \anonymous.json。Android 端默认 noBackupFilesDir——设备身份不随系统
// 备份迁移；Windows 上语义等价物是 LocalAppData（Roaming 会随漫游配置
// 带到别的机器，恰是 Android 端要避开的），故不用 %APPDATA%。目录与
// 文件由 Go 侧 filestore 在首次保存时自动创建（0700/0600）。

#include "include/openhongguo/openhongguo_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <windows.h>
#include <shlobj_core.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// ---------- openhongguo.dll 的 C ABI（导出表见 BUILD_DLL.md） ----------

using OHGVersionFn = char *(*)();
using OHGCreateClientFn = char *(*)(const char *profile_path,
                                    const char *config_json);
using OHGCallFn = char *(*)(int client, const char *method,
                            const char *args_json);
using OHGReleaseClientFn = void (*)(int client);
using OHGFreeStringFn = void (*)(char *s);

// 与 Kotlin 端 INVALID_OPTIONS 文案一致，Dart 端按错误码映射。
const char kReleasedClientJson[] =
    "{\"error\":{\"code\":\"INVALID_OPTIONS\",\"message\":\"openhongguo: "
    "client was released\",\"retryable\":false,\"retryAfterMs\":0}}";

std::string WideToUtf8(const std::wstring &wide) {
  if (wide.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                       static_cast<int>(wide.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) {
    return std::string();
  }
  std::string out(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                      &out[0], size, nullptr, nullptr);
  return out;
}

std::string JsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

std::string ErrorJson(const std::string &code, const std::string &message) {
  return "{\"error\":{\"code\":\"" + JsonEscape(code) +
         "\",\"message\":\"" + JsonEscape(message) +
         "\",\"retryable\":false,\"retryAfterMs\":0}}";
}

// 从 OHGCreateClient 的 {"id":N}（Go json.Marshal 紧凑格式）提取客户端
// 编号；错误形状 {"error":…} 不含 "id":，返回 -1。
int ClientIdOf(const char *json) {
  if (json == nullptr) {
    return -1;
  }
  const std::string marker = "\"id\":";
  const size_t at = std::string(json).find(marker);
  if (at == std::string::npos) {
    return -1;
  }
  return atoi(json + at + marker.size());
}

// 本插件 DLL 所在目录。openhongguo.dll 由 CMake 的 bundled_libraries
// 与插件 DLL 一起拷到应用 exe 旁，标准插件打包布局。
std::wstring PluginModuleDir() {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&PluginModuleDir),
                         &module) == 0 ||
      module == nullptr) {
    return std::wstring(L".");
  }
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
    return std::wstring(L".");
  }
  const std::wstring dir(path);
  const size_t slash = dir.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring(L".")
                                     : dir.substr(0, slash);
}

// 宿主 exe 版本资源里的字符串（CompanyName / ProductName），取法与
// path_provider_windows 一致；读不到返回空。
std::wstring ExeVersionString(const wchar_t *name) {
  wchar_t exe[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
    return std::wstring();
  }
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(exe, &handle);
  if (size == 0) {
    return std::wstring();
  }
  std::vector<BYTE> data(size);
  if (GetFileVersionInfoW(exe, handle, size, data.data()) == 0) {
    return std::wstring();
  }
  struct LangAndCodePage {
    WORD language;
    WORD code_page;
  } *translations = nullptr;
  UINT count = 0;
  if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                     reinterpret_cast<LPVOID *>(&translations),
                     &count) == 0 ||
      translations == nullptr || count < sizeof(LangAndCodePage)) {
    return std::wstring();
  }
  wchar_t sub_block[64] = {};
  swprintf(sub_block, 64, L"\\StringFileInfo\\%04x%04x\\%s",
           translations[0].language, translations[0].code_page, name);
  wchar_t *value = nullptr;
  UINT value_len = 0;
  if (VerQueryValueW(data.data(), sub_block,
                     reinterpret_cast<LPVOID *>(&value),
                     &value_len) == 0 ||
      value == nullptr) {
    return std::wstring();
  }
  return std::wstring(value);
}

// 默认设备档案目录（UTF-8），见文件头注释。
std::string DefaultProfileDirUtf8() {
  std::wstring dir;
  PWSTR base = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                     &base)) &&
      base != nullptr) {
    dir = base;
    CoTaskMemFree(base);
  } else {
    wchar_t fallback[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", fallback, MAX_PATH) > 0) {
      dir = fallback;
    }
  }
  if (dir.empty()) {
    dir = L".";
  }
  const std::wstring company = ExeVersionString(L"CompanyName");
  const std::wstring product = ExeVersionString(L"ProductName");
  if (!company.empty()) {
    dir += L"\\" + company;
  }
  if (!product.empty()) {
    dir += L"\\" + product;
  }
  if (company.empty() && product.empty()) {
    dir += L"\\openhongguo";
  }
  dir += L"\\openhongguo";
  return WideToUtf8(dir);
}

// ---------- 调用线程池 ----------

// OHGCall 是同步阻塞调用，全部丢到后台线程执行；platform 线程只做记账与
// 应答。
//
// **为什么是池而不是单个 worker**：Dart 侧本来就会并发发起调用（详情页
// 同时取推荐 / 多季 / 同类型频道，搜索页并行探测季数），Android 端每个
// 调用各起一个 goroutine（bindings/mobile/core/core.go 的
// Client.Start），SDK 也声明「可以在多个线程里同时使用」。原先的单 worker
// 把这些并发请求排成 FIFO，总耗时变成各次往返之和（Windows 上详情页
// 4 个请求串行 ≈ 4 倍单次延迟），且一次卡住的调用会堵住其后所有请求。
// 这里改为固定大小线程池，与 Android 的并发度对齐。
class WorkerPool final {
 public:
  using Task = std::function<void()>;

  explicit WorkerPool(size_t size) : size_(size == 0 ? 1 : size) {}
  ~WorkerPool() { Stop(); }
  WorkerPool(const WorkerPool &) = delete;
  WorkerPool &operator=(const WorkerPool &) = delete;

  void Start() {
    for (size_t i = 0; i < size_; i++) {
      threads_.emplace_back(&WorkerPool::Run, this);
    }
  }

  void Post(Task task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        return;
      }
      queue_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

  // 停机并 join。队列中尚未执行的任务被丢弃（仅发生在引擎销毁/进程退出
  // 路径，对应的 Dart Future 随 isolate 一起消失；DLL 内的客户端由 Go
  // GC 连同在途调用一起回收，无泄漏风险）。
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

 private:
  void Run() {
    for (;;) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) {
          return;
        }
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      task();
    }
  }

  const size_t size_;
  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Task> queue_;
  bool stopping_ = false;
};

}  // namespace

// ---------- 插件实现 ----------

namespace {

// 一个原生客户端的生命周期记账。
struct ClientEntry {
  explicit ClientEntry(int client_id) : id(client_id) {}

  const int id;

  // 在途 OHGCall 计数：platform 线程受理 call 时 +1，worker 完成时 -1。
  std::atomic<int> in_flight{0};

  // 下面的字段由 mutex 保护。
  std::mutex mutex;
  bool pending_release = false;
  std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
      release_result;
};

int GetIntArg(const flutter::EncodableMap &args, const char *key) {
  const auto it = args.find(flutter::EncodableValue(key));
  if (it == args.end()) {
    return 0;
  }
  const flutter::EncodableValue &value = it->second;
  if (const auto *i = std::get_if<int32_t>(&value)) {
    return *i;
  }
  if (const auto *i = std::get_if<int64_t>(&value)) {
    return static_cast<int>(*i);
  }
  if (const auto *d = std::get_if<double>(&value)) {
    return static_cast<int>(*d);
  }
  return 0;
}

std::string GetStringArg(const flutter::EncodableMap &args, const char *key) {
  const auto it = args.find(flutter::EncodableValue(key));
  if (it == args.end()) {
    return std::string();
  }
  if (const auto *s = std::get_if<std::string>(&it->second)) {
    return *s;
  }
  return std::string();
}

}  // namespace

namespace {

// 插件外壳：生命周期归 registrar 管（AddPlugin），全部实现细节在 Impl。
class Plugin final : public flutter::Plugin {
 public:
  Plugin(flutter::BinaryMessenger *messenger,
         std::string default_profile_dir);

  ~Plugin() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace

class Plugin::Impl final {
 public:
  Impl(flutter::BinaryMessenger *messenger,
       std::string default_profile_dir)
      : channel_(messenger, "dev.openhongguo/flutter",
                 &flutter::StandardMethodCodec::GetInstance()),
        default_profile_dir_(std::move(default_profile_dir)) {
    LoadDll();
    channel_.SetMethodCallHandler(
        [this](const flutter::MethodCall<flutter::EncodableValue> &call,
               std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                   result) {
          HandleMethodCall(call, std::move(result));
        });
    worker_.Start();
  }

  ~Impl() {
    // 先摘掉 channel handler（不再受理新调用），再停 worker
    // （join 掉在途任务），最后清点残留客户端。
    channel_.SetMethodCallHandler(nullptr);
    worker_.Stop();
    std::unordered_map<int, std::shared_ptr<ClientEntry>> clients;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      clients.swap(clients_);
    }
    if (release_client_ != nullptr) {
      for (const auto &pair : clients) {
        release_client_(pair.first);
      }
    }
  }

 private:
  void LoadDll() {
    const std::wstring path = PluginModuleDir() + L"\\openhongguo.dll";
    HMODULE lib = LoadLibraryW(path.c_str());
    if (lib == nullptr) {
      lib = LoadLibraryW(L"openhongguo.dll");
    }
    if (lib == nullptr) {
      dll_error_ = "LoadLibrary openhongguo.dll failed, GetLastError=" +
                   std::to_string(GetLastError());
      return;
    }
    create_client_ = reinterpret_cast<OHGCreateClientFn>(
        GetProcAddress(lib, "OHGCreateClient"));
    call_ = reinterpret_cast<OHGCallFn>(GetProcAddress(lib, "OHGCall"));
    release_client_ = reinterpret_cast<OHGReleaseClientFn>(
        GetProcAddress(lib, "OHGReleaseClient"));
    free_string_ = reinterpret_cast<OHGFreeStringFn>(
        GetProcAddress(lib, "OHGFreeString"));
    if (create_client_ == nullptr || call_ == nullptr ||
        release_client_ == nullptr || free_string_ == nullptr) {
      dll_error_ = "openhongguo.dll is missing OHG_* exports";
      create_client_ = nullptr;
      call_ = nullptr;
      release_client_ = nullptr;
      free_string_ = nullptr;
      FreeLibrary(lib);
    }
  }

  // DLL 返回的 char* 拷贝成 std::string 后立即归还（OHGFreeString）。
  std::string TakeString(char *s) {
    std::string out = (s != nullptr) ? s : "";
    if (s != nullptr && free_string_ != nullptr) {
      free_string_(s);
    }
    return out;
  }

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
          result) {
    const flutter::EncodableValue *arguments = call.arguments();
    flutter::EncodableMap args;
    if (arguments != nullptr) {
      if (const auto *map = std::get_if<flutter::EncodableMap>(arguments)) {
        args = *map;
      }
    }
    if (call.method_name() == "createClient") {
      HandleCreateClient(args, std::move(result));
    } else if (call.method_name() == "call") {
      HandleCall(args, std::move(result));
    } else if (call.method_name() == "releaseClient") {
      HandleReleaseClient(args, std::move(result));
    } else {
      result->NotImplemented();
    }
  }

  // platform 线程直接调（不发网络请求）。profilePath 缺省时落到
  // %LOCALAPPDATA% 下的应用私有目录；log 恒按 false 处理——DLL 未导出
  // 日志回调（RESULTS §4 选项①），Dart 端默认即 log=false，不受影响。
  void HandleCreateClient(
      const flutter::EncodableMap &args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
          result) {
    if (create_client_ == nullptr) {
      result->Success(flutter::EncodableValue(
          ErrorJson("UNKNOWN", dll_error_.empty()
                                   ? "openhongguo.dll not loaded"
                                   : dll_error_)));
      return;
    }
    std::string profile_path = GetStringArg(args, "profilePath");
    if (profile_path.empty()) {
      profile_path = default_profile_dir_ + "\\anonymous.json";
    }
    char *out =
        create_client_(profile_path.c_str(), GetStringArg(args, "config").c_str());
    const int id = ClientIdOf(out);
    result->Success(flutter::EncodableValue(TakeString(out)));
    if (id > 0) {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      clients_[id] = std::make_shared<ClientEntry>(id);
    }
  }

  // OHGCall 丢 worker 线程池执行（同步阻塞最长约 5 分钟）。
  void HandleCall(
      const flutter::EncodableMap &args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
          result) {
    const int client = GetIntArg(args, "client");
    std::shared_ptr<ClientEntry> entry;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      const auto it = clients_.find(client);
      if (it != clients_.end()) {
        entry = it->second;
      }
    }
    if (entry == nullptr || call_ == nullptr) {
      // 与 Kotlin/DLL 同文案：客户端不存在（或插件未就绪）按已释放报。
      result->Success(flutter::EncodableValue(std::string(kReleasedClientJson)));
      return;
    }
    entry->in_flight.fetch_add(1);
    const std::string method = GetStringArg(args, "method");
    const std::string call_args = GetStringArg(args, "args");
    // 任务队列用 std::function（要求可拷贝），把应答对象收进 shared_ptr；
    // Success/Error 是 MethodResult 的公开接口，与 unique_ptr 用法一致。
    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> result_ptr(
        result.release());
    worker_.Post([this, entry, method, call_args, result_ptr]() {
      char *out = call_(entry->id, method.c_str(), call_args.c_str());
      // 应答通道线程安全（见文件头注释），等价于回投 platform 线程后
      // result.success(out)。
      result_ptr->Success(flutter::EncodableValue(TakeString(out)));
      FinishInFlight(entry);
    });
  }

  // 先排空再删：摘除注册（新 call 立即报 released）；有在途调用时把
  // 应答挂起，由最后一个在途调用完成后代调 OHGReleaseClient 并代答。
  // 与 Kotlin 端一致，重复 release 也按成功返回。
  void HandleReleaseClient(
      const flutter::EncodableMap &args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
          result) {
    const int client = GetIntArg(args, "client");
    std::shared_ptr<ClientEntry> entry;
    {
      std::lock_guard<std::mutex> lock(clients_mutex_);
      const auto it = clients_.find(client);
      if (it != clients_.end()) {
        entry = it->second;
        clients_.erase(it);
      }
    }
    if (entry == nullptr) {
      result->Success(nullptr);
      return;
    }
    bool drained_now = false;
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      entry->pending_release = true;
      if (entry->in_flight.load() == 0) {
        drained_now = true;
      } else {
        entry->release_result = std::move(result);
      }
    }
    if (drained_now) {
      if (release_client_ != nullptr) {
        release_client_(entry->id);
      }
      result->Success(nullptr);
    }
  }

  // worker 线程：一次 OHGCall 收尾。归零且已挂 release 时补完释放。
  void FinishInFlight(const std::shared_ptr<ClientEntry> &entry) {
    bool drained = false;
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      entry->in_flight.fetch_sub(1);
      drained = entry->pending_release && entry->in_flight.load() == 0;
    }
    if (!drained) {
      return;
    }
    if (release_client_ != nullptr) {
      release_client_(entry->id);
    }
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result;
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      result = std::move(entry->release_result);
    }
    if (result != nullptr) {
      result->Success(nullptr);
    }
  }

  flutter::MethodChannel<flutter::EncodableValue> channel_;
  const std::string default_profile_dir_;

  OHGCreateClientFn create_client_ = nullptr;
  OHGCallFn call_ = nullptr;
  OHGReleaseClientFn release_client_ = nullptr;
  OHGFreeStringFn free_string_ = nullptr;
  std::string dll_error_;

  // worker 线程池规模：与 Android 端「每次调用一个 goroutine」的并发度
  // 对齐，同时限制单进程内的并发请求数（Go 侧 DefaultTransport 对同一
  // 主机的空闲连接上限也是 2，池再大也不能更快）。
  static constexpr size_t kWorkerCount = 4;

  WorkerPool worker_{kWorkerCount};
  std::mutex clients_mutex_;
  std::unordered_map<int, std::shared_ptr<ClientEntry>> clients_;
};

// ---------- 公开入口（generated_plugin_registrant.cc 调用） ----------

Plugin::Plugin(flutter::BinaryMessenger *messenger,
               std::string default_profile_dir)
    : impl_(std::make_unique<Impl>(messenger, std::move(default_profile_dir))) {}

Plugin::~Plugin() = default;

void OpenhongguoPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  // C API ref → C++ wrapper registrar（引擎保证 registrar 存活期），
  // 与 window_manager 等官方系插件同一注册结构。
  flutter::PluginRegistrarWindows *plugin_registrar =
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar);
  plugin_registrar->AddPlugin(std::make_unique<Plugin>(
      plugin_registrar->messenger(), DefaultProfileDirUtf8()));
}
