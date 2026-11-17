# openhongguo.dll 的再生方法

`windows/openhongguo.dll`（7,380,992 字节，sha256
`63a906132a71ede32703c874993ea4e09d224dfb701f42879b912cc1fed9025`）是
[openhongguo Go SDK](https://github.com/beilunyang/openhongguo) 经
cgo `-buildmode=c-shared` 编出的 Windows x64 产物，以二进制入库
（约 7 MB）。SDK 升级时按以下步骤再生。

## 前置

- Go 1.26+（全路径使用即可，不必装入系统 PATH）
- mingw-w64 gcc（c-shared 硬依赖外部链接器；MSVC 不被 cgo 支持），
  例如 niXman mingw-builds 的 x86_64-posix-seh-ucrt 版本
- cgo 导出层源码在本仓库 `bindings/windowspoc/`（模块
  `github.com/beilunyang/openhongguo/bindings/windowspoc`；模块路径取
  `bindings/` 前缀才能导入 `internal` 包——Go 的 internal 包规则按
  导入路径的前缀判断）

## 步骤（Git Bash）

```bash
cd <本仓库>/bindings/windowspoc
export GOPROXY=https://goproxy.cn,direct
export CGO_ENABLED=1   # 若 go env 持久化了 CGO_ENABLED=0，必须覆盖
export CC="<mingw64/bin/gcc.exe 全路径>"
go build \
  -buildmode=c-shared -trimpath -ldflags="-s -w" \
  -o ../flutter/windows/openhongguo.dll .
```

CI 备选：Linux 容器交叉编译
`GOOS=windows CGO_ENABLED=1 CC=x86_64-w64-mingw32-gcc go build …`。

## 导出表（cgo 自动生成 openhongguo.h，调用方 cdecl）

```c
char* OHGVersion(void);
char* OHGCreateClient(const char* profilePath, const char* configJSON);
char* OHGCall(int client, const char* method, const char* argsJSON);
void  OHGReleaseClient(int client);
void  OHGFreeString(char* s);
```

仅依赖 KERNEL32 + UCRT api-ms-win-crt-*（Win10+ 系统自带，无需随包
分发 mingw 运行时）。验证：`objdump -p openhongguo.dll | tail -20`。

## 约束

- 再生后 DLL 与 `openhongguo_plugin.cpp` 的 C ABI 对应关系不变才能直接
  替换；若 SDK 增删导出（如日志回调），需同步改插件实现；
- 产物不含密钥/设备档案等敏感数据，可入库；设备档案运行时落在
  `%LOCALAPPDATA%`，绝不入库。
