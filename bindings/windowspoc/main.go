// Package main 把 openhongguo SDK 编译成 Windows 的 c-shared DLL（桌面移植 POC）。
//
// 导出的 C ABI 与 Flutter 插件的 MethodChannel 'dev.openhongguo/flutter'
// 协议一一对应（client.dart 的 createClient/call/releaseClient）：
//
//	const char* OHGVersion(void);
//	char* OHGCreateClient(const char* profilePath, const char* configJSON);
//	char* OHGCall(int client, const char* method, const char* argsJSON);
//	void OHGReleaseClient(int client);
//	void OHGFreeString(char* s);
//
// 所有返回 JSON 字符串的函数，结果由调用方用 OHGFreeString 释放。
// JSON 形状与移动端一致：{"id":n} / {"result":...} / {"error":{code,
// message, retryable, retryAfterMs}}。方法名和参数数组与 dispatch 层一致
// （discover、search、resolvePlayback 等 25 个方法）。
//
// OHGCall 是同步调用（POC 简化）；W2 的 Flutter 插件应在 worker 线程调用。
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"context"
	"encoding/json"
	"runtime"
	"sync"
	"time"
	"unsafe"

	"github.com/beilunyang/openhongguo/adapters"
	"github.com/beilunyang/openhongguo/bindings/internal/dispatch"
	"github.com/beilunyang/openhongguo/sdk"
)

var (
	mu      sync.Mutex
	nextID  int
	clients = map[int]*sdk.Client{}
)

func goStr(s *C.char) string {
	if s == nil {
		return ""
	}
	return C.GoString(s)
}

func outJSON(v any) *C.char {
	b, err := json.Marshal(v)
	if err != nil {
		b, _ = json.Marshal(map[string]any{
			"error": map[string]any{"code": "UNKNOWN", "message": err.Error(), "retryable": false, "retryAfterMs": 0},
		})
	}
	return C.CString(string(b))
}

func errJSON(err error) *C.char {
	return outJSON(map[string]any{"error": dispatch.Error(err)})
}

//export OHGVersion
func OHGVersion() *C.char {
	return C.CString("openhongguo-windows-poc/0.1 (" + runtime.Version() + ", " + runtime.GOOS + "/" + runtime.GOARCH + ")")
}

//export OHGCreateClient
func OHGCreateClient(profilePath, configJSON *C.char) *C.char {
	cfg, err := dispatch.ParseConfig(goStr(configJSON))
	if err != nil {
		return errJSON(err)
	}
	var opts sdk.Options
	cfg.Apply(&opts)
	c, err := adapters.NewClient(goStr(profilePath), opts)
	if err != nil {
		return errJSON(err)
	}
	mu.Lock()
	nextID++
	id := nextID
	clients[id] = c
	mu.Unlock()
	return outJSON(map[string]any{"id": id})
}

//export OHGCall
func OHGCall(id C.int, method, argsJSON *C.char) *C.char {
	mu.Lock()
	c := clients[int(id)]
	mu.Unlock()
	if c == nil {
		return outJSON(map[string]any{"error": map[string]any{
			"code": "INVALID_OPTIONS", "message": "openhongguo: client was released",
			"retryable": false, "retryAfterMs": 0,
		}})
	}
	// 5 分钟足够覆盖默认的重试和自动换设备（retryAttempts=3, deviceResets=2）。
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()
	v, err := dispatch.Call(ctx, c, goStr(method), goStr(argsJSON))
	if err != nil {
		return errJSON(err)
	}
	return outJSON(map[string]any{"result": v})
}

//export OHGReleaseClient
func OHGReleaseClient(id C.int) {
	mu.Lock()
	delete(clients, int(id))
	mu.Unlock()
}

//export OHGFreeString
func OHGFreeString(s *C.char) {
	if s != nil {
		C.free(unsafe.Pointer(s))
	}
}

func main() {}
