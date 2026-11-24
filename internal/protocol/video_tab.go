package protocol

import "strconv"

// video_tab 是 V5 播放接口之外的第三条取址通道（fork 自 PR#16 移植）。
// 当 V5 主路径对某些设备或剧集返回空模型时，这条无请求体的 GET 通道
// 仍可能返回完整模型；响应在 data.video_model_data[episodeID].video_model，
// 由 ParsePlayback 的 video_model_data 分支解析。

const (
	// PathVideoTab 是 video_tab 取址接口。
	PathVideoTab = "/reading/bookapi/video_tab/multi_video_model/v1/"
	// VideoTabUserAgent 是拍摄自官方客户端的 video_tab 请求 UA（固定模板，
	// 只有匿名身份与集 ID 随请求变化）。
	VideoTabUserAgent = "com.phoenix.read/73932 (Linux; U; Android 16; zh_CN; 25053RT47C; Build/BP2A.250605.031.A3; Cronet/TTNetVersion:04657795 2026-01-23 QuicVersion:c67e9834 2025-09-08)"
)

// VideoTabPairs 是 video_tab 的无请求体查询，参数顺序属于签名的一部分。
func VideoTabPairs(episodeID string, id SearchIdentity) [][2]string {
	return [][2]string{
		{"aid", AID},
		{"app_name", AppName},
		{"version_code", strconv.Itoa(PlaybackVersionCode)},
		{"version_name", PlaybackVersionName},
		{"channel", "vivo_8662_64"},
		{"device_platform", "android"},
		{"device_type", "PGT-AN10"},
		{"os_version", "12"},
		{"update_version_code", strconv.Itoa(PlaybackVersionCode)},
		{"iid", id.InstallID},
		{"cdid", id.CDID},
		{"device_id", id.DeviceID},
		{"video_id", episodeID},
		{"video_id_type", "0"},
		{"device_level", "1"},
		{"need_all_video_definition", "1"},
		{"need_mp4_align", "0"},
		{"use_os_player", "0"},
		{"video_platform", "1024"},
		{"source", "0"},
		{"from_video_id", ""},
	}
}
