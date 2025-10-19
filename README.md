# rtspunch

实验性程序，访问 stun 服务器打洞，将映射的公网端口写入rtsp控制报文，接收 rtp 数据，返回给客户端。 

## 命令行参数

### 可选参数

```
-p, –port                 设置服务器监听端口
-n, –enable-nat           启用 NAT 穿透支持
-r, –set-rtp-buffer-size  设置最大 RTP 缓冲区大小（字节）
-u, –set-max-udp-packet-size 设置最大 UDP 数据包大小（字节）
```

### 参数示例

```bash
./rtspunch -p 8080 -n --set-rtp-buffer-size 16384 --set-max-udp-packet-size 1500
```

### 访问地址

`rtsp://192.168.0.1:1554` -> `http://ip:port/rtp/192.168.0.1:1554`
