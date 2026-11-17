module github.com/beilunyang/openhongguo/bindings/windowspoc

go 1.22

require github.com/beilunyang/openhongguo v0.0.0

// replace 指向只读的 SDK 源码仓库；POC 不修改 SDK 的任何文件。
// 模块路径取 bindings/ 前缀，才能导入 bindings/internal/dispatch（Go 的
// internal 包规则按导入路径的前缀判断）。
replace github.com/beilunyang/openhongguo => ../..
