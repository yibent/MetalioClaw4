# SD 卡资源说明

本目录仅保留聊天页的 EAF 表情资源。将 `system/` 目录复制到
FAT 格式 SD 卡根目录，并保持目录结构不变。

运行时路径为：

```text
/sdcard/system/chat/{emotion}.eaf
```

`{emotion}` 由服务端下发，对应 `system/chat/` 中的同名文件。
