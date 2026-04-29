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
- `spawn_stream_child(...)`

退出时会调用：

- `stop_mqtt_reporter()`
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

### 6. 离线缓存逻辑

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

### 7. SQLite 运行策略

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

### 3. 调试接口

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

### 3. 断网恢复测试建议

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

- 当前正式能力只覆盖“结构化数据离线缓存和补发”，还不包括“视频文件本体离线缓存与补传”。
- 当前 RTMP 恢复能力只覆盖“实时推流自动重连”，还不包括“断网期间视频片段落盘与后续补传”。

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

## 当前状态

已经验证通过的能力：

- SQLite 本地建库
- 断网时结构化数据落盘
- 恢复联网后 MQTT 自动补发
- 补发成功后删除离线记录
- RTMP 写失败时推流子进程退出
- 父进程在 RTMP 掉线后继续运行
- 父进程按指数退避策略重启推流子进程

还未正式接入的能力：

- 视频文件本体落 SD 卡
- 视频文件路径写入 `media_path`
- 视频恢复联网后的文件级补传
