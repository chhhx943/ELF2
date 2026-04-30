# ELF2 Camera Flow

这个工程当前包含三条正式业务链路：

- 视频采集与 RTMP 推流
- Modbus 传感器采集
- MQTT 周期上报与断网离线缓存

## 正式逻辑

### 1. 主程序结构

`main.c` 在启动后会 `fork()` 成两个进程：

- 子进程：负责把摄像头 DMA-BUF 送进编码器并推到 RTMP
- 父进程：负责摄像头取帧、启动传感器线程、启动 MQTT 线程
- 父进程同时维护推流子进程状态，负责在推流掉线后按指数退避策略重启子进程

父进程里当前会启动：

- `start_sensor_collector(...)`
- `start_mqtt_reporter()`
- `video_uploader_start(...)`（仅当 `video_uploader.h` 里的上传宏配置齐全时）
- `spawn_stream_child(...)`

退出时会调用：

- `stop_mqtt_reporter()`
- `video_uploader_stop(...)`
- `stop_stream_child(...)`

### 2. 传感器链路

`sensor_modbus.c` 持续采集：

- `ppm`
- `temp`
- `humi`
- `alarm_status`
- `run_status`

采集结果写入全局 `g_sensor_data`，由 `pthread_mutex_t lock` 保护。

### 3. MQTT 正式行为

`aliyun_mqtt.c` 里的正式线程使用：

- `timerfd`
- `eventfd`
- `poll`

每 `10` 秒触发一次上报周期。

每次周期内的顺序是：

1. 从 `g_sensor_data` 加锁读取一份快照
2. 生成 MQTT payload
3. 如果当前未连接，则先尝试连接阿里云 MQTT
4. 如果连接成功且本地 SQLite 里有离线数据，先补发离线数据
5. 再发送当前实时数据
6. 如果发送失败，则把当前数据写入本地 SQLite

这样恢复联网后，正式线程会在当次周期内优先补发离线数据，而不是继续只发实时数据。

### 4. RTMP 正式行为

`main.c` 里的父子进程推流链路当前采用“掉线降级、指数退避重启”的策略：

1. 父进程正常取帧
2. 如果推流子进程在线，则通过 `send_fd()` 把 DMA-BUF 传给子进程
3. 父进程等待子进程返回 `ack`
4. 如果 `send_fd()` 或 `ack` 失败，父进程不会退出，而是把推流状态标记为离线
5. 即使推流离线，父进程仍然继续采集并把当前帧回队给驱动
6. 父进程每隔一段时间尝试重新拉起推流子进程，重启间隔按指数退避增长

这意味着 RTMP 断网时：

- 推流子进程可以退出
- 父进程不会因为推流失败而结束
- 传感器和 MQTT/SQLite 离线缓存仍然继续工作

当前默认退避节奏为：

- 初始重启间隔：`1s`
- 之后按 `1s -> 2s -> 4s -> 8s -> 16s -> 30s` 增长
- 达到 `30s` 后维持在 `30s`
- 一旦某次重启成功，退避间隔重置回 `1s`

### 5. 编码器/推流错误处理

`encoder.c` 当前已经开始检查 RTMP 关键写路径的返回值：

- `avformat_write_header(...)`
- `av_interleaved_write_frame(...)`

一旦 RTMP 写失败，编码函数会返回错误，子进程会退出。父进程随后会把推流状态切成离线，并进入定时重启流程。

### 6. 视频断网落盘逻辑

当前已经接入一个最小闭环版本的视频本地转存逻辑：

- 子进程启动时优先尝试 RTMP 输出
- 如果 RTMP 初始化失败，立即切到本地 `.ts` 分片模式
- 如果 RTMP 运行中写失败，当前子进程不退出，而是切到本地 `.ts` 分片模式继续写文件
- 本地文件模式下会按固定片段时长轮转
- 每个片段都会在 SQLite 的 `video_segments` 表中登记索引
- 超过容量阈值时自动删除最旧片段
- 到达片段轮转点时，会顺带尝试恢复 RTMP，成功后切回实时推流

当前默认参数：

- 片段时长：`30s`
- 总容量上限目标：约 `3GB`
- 高水位：`2.8GB`
- 低水位：`2.5GB`

默认视频目录在：

```bash
$CAMERA_FLOW_STORE_DIR/video/YYYY-MM-DD/
```

SQLite 里的视频索引表为：

- `video_segments`

### 7. 视频补传正式行为

`video_uploader.c` 当前已经正式接入 `main.c` 的父进程生命周期。

行为如下：

- 主程序启动后，会检查 `video_uploader.h` 中以下宏是否为空
  - `VIDEO_UPLOADER_HTTP_UPLOAD_URL`
  - `VIDEO_UPLOADER_HTTP_AUTH_TOKEN`
  - `VIDEO_UPLOADER_HTTP_DEVICE_ID`
- 三个宏都非空时，父进程会启动后台 `video_uploader` 线程
- 线程会从 `video_segments` 表中按时间顺序抓取 `state='pending'` 的片段
- 上传成功后，把该片段状态更新为 `uploaded`
- 上传失败后，把状态改回 `pending`，并增加 `retry_count`
- 如果宏配置不完整，主程序不会退出，而是打印 `Video uploader skipped: ...` 后继续跑采集、RTMP、传感器、MQTT 主链路

当前 HTTP 上传回调会用 `multipart/form-data` 发送：

- `device_id`
- `segment_id`
- `start_ms`
- `end_ms`
- `size_bytes`
- `file`

并带请求头：

- `Authorization: Bearer VIDEO_UPLOADER_HTTP_AUTH_TOKEN`

服务端返回值当前要求：

- HTTP 状态码 `200`
- JSON 中 `code=0`
- JSON 中包含 `remote_path`

常用查看命令：

```bash
sqlite3 /media/elf/2461-4BDA/flow/offline.db "SELECT id, start_ms, end_ms, size_bytes, state, file_path FROM video_segments ORDER BY id DESC LIMIT 10;"
```

### 8. 离线缓存逻辑

本地缓存由 `local_store.c/.h` 提供，底层使用 SQLite。

默认数据库目录：

- `/media/elf/2461-4BDA/camera_flow`

默认数据库文件：

- `/media/elf/2461-4BDA/camera_flow/offline.db`

也可以通过环境变量覆盖：

- `CAMERA_FLOW_STORE_DIR`

SQLite 表当前保存：

- `created_at_ms`
- `topic`
- `payload`
- `ppm`
- `temp`
- `humi`
- `alarm_status`
- `media_type`
- `media_path`

其中：

- `payload` 直接保存最终 MQTT JSON，恢复联网后可以原样补发
- `media_type/media_path` 先预留给后续视频或图片文件索引

### 9. SQLite 运行策略

当前数据库初始化时会设置：

- `PRAGMA journal_mode=WAL`
- `PRAGMA synchronous=NORMAL`

并且本地缓存队列有上限：

- `100000` 条

超过上限时会删除最旧记录，避免 SD 卡被离线数据持续写满。

## 测试辅助

当前保留了两套独立测试工具，它们不需要跑主流程就能验证数据库和离线补发链路。

### 1. `store_debug`

用途：

- 单独验证 SQLite 打开、建表、插入、查询、删除

构建：

```bash
make store_debug
```

用法：

```bash
./store_debug selftest [root_dir]
./store_debug dump [root_dir] [limit]
```

说明：

- `selftest` 会插入一条测试数据，再读出、删除、校验数量
- `dump` 会打印当前离线库里的记录

### 2. `mqtt_chain_debug`

用途：

- 验证“模拟断网 -> 写 SQLite -> 恢复联网 -> MQTT 补发 -> 删除离线记录”整条链路

构建：

```bash
make mqtt_chain_debug
```

用法：

```bash
./mqtt_chain_debug offline-test [root_dir]
./mqtt_chain_debug dump [root_dir] [limit]
./mqtt_chain_debug flush-test [root_dir]
./mqtt_chain_debug full-test [root_dir]
```

说明：

- `offline-test`
  - 强制开启 `force_offline`
  - 模拟 MQTT 发送失败
  - 当前数据直接写入 SQLite

- `flush-test`
  - 关闭 `force_offline`
  - 尝试连接 MQTT
  - 从 SQLite 取出待补发记录
  - 发送成功后删除离线记录

- `full-test`
  - 连续执行一次完整闭环验证

### 3. `video_uploader_debug`

用途：

- 单独验证视频片段补传线程
- 支持本地 mock 上传和真实 HTTP 上传两种模式

构建：

```bash
make video_uploader_debug
```

用法：

```bash
./video_uploader_debug mock [root_dir]
./video_uploader_debug http [root_dir]
```

说明：

- `mock`
  - 不访问网络
  - 把待上传分片复制到 `root_dir/uploaded_mock/`
  - 默认会模拟一次瞬时失败，验证自动重试

- `http`
  - 调用真实的 `video_uploader_http_upload_callback(...)`
  - 会发起 multipart HTTP 上传
  - 使用 `video_uploader.h` 中写死的上传宏配置

本地联调用法：

```bash
make video_uploader_debug
./video_uploader_debug http /tmp/video_uploader_http_test
```

正常情况下应看到：

- 控制台打印 `[VideoUploader] Uploaded segment ...`
- `video_segments.state` 变成 `uploaded`
- `remote_path` 被回写进 SQLite

可用下面命令检查：

```bash
sqlite3 /tmp/video_uploader_http_test/offline.db "SELECT id, state, retry_count, remote_path, last_error FROM video_segments ORDER BY id;"
```

如果只是先验证服务端 API，也可以直接用 curl：

```bash
printf 'test\n' > /tmp/test.ts
curl -F "device_id=0122" \
     -F "segment_id=1" \
     -F "start_ms=1777600000000" \
     -F "end_ms=1777600030000" \
     -F "size_bytes=5" \
     -F "file=@/tmp/test.ts" \
     -H "Authorization: Bearer CHANGE_ME_TOKEN" \
     http://127.0.0.1/api/video/upload
```

### 4. 调试接口

`aliyun_mqtt.h` 当前还暴露了以下测试接口：

- `mqtt_debug_set_force_offline(int enabled)`
- `mqtt_debug_enqueue_fake_record(const char *root_dir, const char *payload)`
- `mqtt_debug_flush_offline_once(const char *root_dir)`
- `mqtt_debug_run_end_to_end_test(const char *root_dir)`

这几组接口用于测试，不是主流程业务入口。

## 测试手段

推荐把测试拆成两类：

### 1. 逻辑层测试

优先使用 `mqtt_chain_debug` 做回归验证，因为它不依赖真实网络波动，结果最稳定。

典型流程：

```bash
make mqtt_chain_debug
./mqtt_chain_debug full-test /media/elf/2461-4BDA/flow
```

这个测试会验证：

- 模拟断网时 MQTT 不发送
- 当前数据写入 SQLite
- 恢复发送后从 SQLite 取出待补发记录
- MQTT 补发成功后删除离线记录

### 2. 主程序真机测试

主程序测试建议在板子真实联网环境下进行：

```bash
export CAMERA_FLOW_STORE_DIR=/media/elf/2461-4BDA/flow
export SENSOR_MODBUS_DEV=/dev/ttyUSB0
./main
```

运行时可以用 SQLite 命令观察离线库：

```bash
sqlite3 /media/elf/2461-4BDA/flow/offline.db "SELECT COUNT(*) FROM offline_queue;"
sqlite3 /media/elf/2461-4BDA/flow/offline.db "SELECT id, created_at_ms, topic FROM offline_queue ORDER BY id DESC LIMIT 10;"
```

如果没有显式设置 `CAMERA_FLOW_STORE_DIR`，程序会回退到默认目录：

```bash
/media/elf/2461-4BDA/camera_flow/offline.db
```

最可靠的方式是直接看程序启动日志里的这一行：

```text
[离线缓存] SQLite 已就绪: ...
```

日志里打印出的路径，就是当前程序实际使用的数据库路径。

### 3. 视频断网转存测试

当前已经提供一个最小测试接口，使用环境变量触发模拟 RTMP 运行中断开：

```bash
export CAMERA_FLOW_STORE_DIR=/media/elf/2461-4BDA/flow
export CAMERA_FLOW_VIDEO_SEGMENT_MS=5000
export CAMERA_FLOW_DEBUG_RTMP_FAIL_AFTER_FRAMES=60
./main
```

推荐这样理解这几个环境变量：

- `CAMERA_FLOW_STORE_DIR`
  - 指定 SQLite 和视频文件根目录
- `CAMERA_FLOW_VIDEO_SEGMENT_MS=5000`
  - 把分片时长临时改成 5 秒，方便快速观察轮转
- `CAMERA_FLOW_DEBUG_RTMP_FAIL_AFTER_FRAMES=60`
  - 子进程在 RTMP 模式下处理到第 60 帧时，主动模拟一次 RTMP 断开

如果还要顺带验证历史视频片段补传，先改好 `video_uploader.h` 里的上传宏配置，这样 `main` 中的 `video_uploader` 线程会自动启动并补传 `pending` 片段。

测试时你应该观察到：

- 日志出现：
  - `[Child] Debug: simulate RTMP disconnect ...`
  - `[Child] RTMP push failed, switch to local file mode.`
  - `[Child] Local segment started: ...`
- SD 卡目录里出现 `.ts` 文件
- `video_segments` 表里出现新纪录

可用下面两组命令观察：

```bash
find /media/elf/2461-4BDA/flow/video -type f -name '*.ts' | sort
sqlite3 /media/elf/2461-4BDA/flow/offline.db "SELECT id, start_ms, end_ms, size_bytes, state, file_path FROM video_segments ORDER BY id DESC LIMIT 10;"
```

### 4. 断网恢复测试建议

如果只是验证应用逻辑，优先用：

```bash
./mqtt_chain_debug full-test ...
```

如果要验证 `main` 在真实场景下的行为，优先使用：

- 关闭路由器外网
- 关闭热点
- 让板子真实脱离 AP 再重新连回

不推荐把 Wi-Fi 主测试手段建立在下面这种命令上：

```bash
ip link set <wifi_if> down
ip link set <wifi_if> up
```

原因是这会把无线接口直接拉掉，恢复后未必会自动重新关联 AP、重新拿 IP、重新拿默认路由。这样测出来的问题往往是系统 Wi-Fi 恢复问题，不一定是应用层 MQTT 重连问题。

## 注意事项

- 当前正式链路已经验证通过：断网时结构化数据会写入 SQLite，恢复联网后会自动补发，并在补发成功后删除离线记录。
- 当前 RTMP 正式链路已经改成：推流断开时子进程可以退出，但父进程不会因为 `send_fd/ack` 失败直接结束，而是继续采集并按指数退避策略重启推流子进程。
- 当前视频断网转存最小闭环已经接通：RTMP 失败后会切到本地 `.ts` 分片，并在 SQLite 中登记 `video_segments` 索引。
- 当前视频补传已经正式接进 `main.c`：上传宏配置齐全时，父进程会自动启动 `video_uploader` 线程。
- 如果主程序日志里已经出现：

```text
[离线缓存] 已保存一条离线记录。
```

但手工查询数据库是 `0`，优先检查是不是查错了数据库路径。

- 当前 MQTT 测试更适合验证“应用层是否能正确落库与补发”，不适合用 Wi-Fi `down/up` 的方式直接判断“系统网络是否能自动恢复”。
- `mqtt_chain_debug` 已经验证通过的闭环是：

```text
模拟断网 -> 写 SQLite -> 恢复联网 -> MQTT 补发 -> 删除离线记录
```

- 当前视频补传能力依赖服务端 HTTP 接口可用；如果上传宏未配置，主程序会主动跳过这条链路。

## 构建

主程序：

```bash
make
```

SQLite 单独测试：

```bash
make store_debug
```

MQTT 离线补发全链路测试：

```bash
make mqtt_chain_debug
```

视频补传调试：

```bash
make video_uploader_debug
```

## 当前状态

已经验证通过的能力：

- SQLite 本地建库
- 断网时结构化数据落盘
- 恢复联网后 MQTT 自动补发
- 补发成功后删除离线记录
- RTMP 写失败时推流子进程退出
- 父进程在 RTMP 掉线后继续运行
- 父进程按指数退避策略重启推流子进程
- RTMP 失败后自动切到本地 `.ts` 分片
- `video_segments` SQLite 索引登记
- `video_uploader` 自动补传 `pending` 视频片段
- 上传成功后回写 `video_segments.state=uploaded`
- 按 3GB 左右容量上限自动清理旧片段

还需要继续完善的能力：

- 服务端鉴权、存储、检索方案按实际部署继续收敛
