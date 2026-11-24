## 0.4.1

- 修复取播放地址得到空结果的问题。服务端从 2026-11-23 起拒绝带模拟器状态值的签名，0.4.0 的播放请求返回 HTTP 200 和空响应体，换设备也不能恢复。
- 内置签名改为与 App 自带的签名库 v04.10.03 一致，签名里的设备环境按真机的格式生成，机型、品牌和系统版本取自匿名设备档案。
- 签名服务 openhongguo-signer 同步修复。使用签名服务的用户需要把镜像更新到 `beilunyang/openhongguo-signer:v0.4.1`。

## 0.4.0

- 新增选项 `signerUrl` 和 `signerToken`（Go 是 `SignerURL`、`SignerToken`），可以把请求签名交给 openhongguo-signer 签名服务。不设时仍用内置签名。签名服务不可用时返回 `SIGNER_UNAVAILABLE`，错误信息带有签名服务给出的原因，不会回退到内置签名。命令行工具用环境变量 `OHG_SIGNER_URL` 和 `OHG_SIGNER_TOKEN`。
- 新增签名服务 openhongguo-signer（仓库的 `signer/` 目录）：在 unidbg 里运行 App 自带的签名库，通过 HTTP 提供签名。镜像发布在 Docker Hub 的 `beilunyang/openhongguo-signer`。
- 本 fork 同步至上游 v0.4.0，播放取址修复见下面的 0.3.1。
- 本 fork 保留预编译原生库（Android `core.jar` 与四个 ABI 的 `libgojni.so`、iOS xcframework），并新增 windows 平台桥接（cgo DLL，再生步骤见 `windows/BUILD_DLL.md`）；上游的 gitignore 规则相应放开。
- `pubspec.yaml` 的 description 保持 ASCII：规避中文 Windows 下 pub 读取 git stdout 按 GBK 重编码导致的 YAML 解析失败。

## 0.3.1

- 播放改走 V5 取址接口，请求参数和签名与 7.4.3.32 版客户端一致。0.3.0 取播放模型失败的问题在这一版修复。
- 读取演员作品时先取作品页的 tab 列表，再按「剧作」tab 请求。不同设备的 tab 编号不同，0.3.0 在部分设备上会得到空列表。
- 搜索联想的关键词参数改为 `q`，与客户端一致。

## 0.3.0

- 新增弹幕：`listDanmaku(seriesId, episodeId, options)` 按播放时间分页读取一集的弹幕，对应 Go SDK 的 `ListDanmaku`（上游 0.3.0）
- 同步上游 main 到 `bf8df22`（弹幕解析、CLI `ohg danmaku`、CI 固定 action 版本）

## 0.2.2

- 搜索结果补上封面：`ParseSearchTab` 早已解析 `video_detail.series_cover`，但构造 `SeriesSummary` 时漏了 `CoverURL`，导致搜索接口返回的条目一律没有封面（客户端只能显示占位图）。`video_detail_list` 分支本来就有映射，现在两者一致。

## 0.2.1

- 播放请求补上完整签名头，修复服务端校验收紧后新匿名设备播放返回空 body 的问题（对应上游 PR #7）。

## 0.2.0

- 设备被拒或播放返回空结果时，客户端自动换一台新的匿名设备并重试这次调用。新选项 `deviceResets` 设置一次调用最多换几台设备，默认 2，0 表示关闭。

## 0.1.0

- 第一个版本。
