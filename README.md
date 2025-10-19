# rtsproxy

## 命令行参数

程序通过命令行参数配置端口、NAT 支持及缓冲区大小等选项。

### 可选参数

| 参数 | 说明 | 类型 | 默认值 |
|------|------|------|--------|
| `-p`, `--port` | 设置服务器监听端口 | 整数 | 8080（示例，实际根据代码默认值） |
| `-n`, `--enable-nat` | 启用 NAT 穿透支持 | 无参数 | 不启用 |
| `-r`, `--set-rtp-buffer-size` | 设置最大 RTP 缓冲区大小（字节） | 整数 | 8192（示例） |
| `-u`, `--set-max-udp-packet-size` | 设置最大 UDP 数据包大小（字节） | 整数 | 1536（示例） |

### 参数示例

```bash
./rtsproxy -p 8080 -n --set-rtp-buffer-size 16384 --set-max-udp-packet-size 1500