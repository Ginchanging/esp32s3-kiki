# 苹果健康接入

使用官方 Home Assistant iPhone App 的 Apple Health 传感器；不需要付费导出 App。链路：

```
Apple Watch / 苹果健康 → iPhone HA App → HTTPS Caddy → 本机 HA
                                                       ↓ 每分钟一次模板查询
                                                     HealthHub
                                                       ↓ 原有 30 秒状态请求
                                                     Kiki 第九页
```

## 配置

1. 将 `ha.kiki-home.top` 的 A 记录设为 hub 的 `8.138.138.222`，使域名满足所在托管环境的公网接入要求。
2. `cd server && ./deploy.sh hub`，然后 `./deploy.sh ha`。HA 保留原有账户、米家和 CalDAV 配置；启用 `mobile_app`。启动时先使用仅发布 `127.0.0.1:8123` 的 Docker 桥接网络；全新安装需按 README 的米家首次设置，经 SSH 隧道创建管理员账户、保存令牌后再次部署。HTTP 设置通过 HA 2026.10 的管理员 API 试运行、重启、验证并确认，监听 `127.0.0.1:8123`，只信任回环代理，确认后才切换到主机网络。旧 YAML `http:` 配置不再用于判断实际监听；验证失败保留隔离的引导容器。
3. `./deploy.sh ha-proxy`。使用 Caddy 官方签名稳定软件源，导入单独的 `/etc/caddy/kiki-ha.caddy`；证书由 Caddy 自动申请、续期。保留其他 Caddy 配置及板子的固定证书 `:8443` 入口。
4. 在 iPhone 官方 HA App 中手动连接 `https://ha.kiki-home.top`，登录已有 HA 账户。在「设置 → Companion App → 传感器 → Apple Health Sensors」启用需要的项目；若 App 版本需要，先在 Labs 开启健康功能。逐项允许读取，不开启无关健康项目。
5. 保持手机解锁，返回 HA 首页下拉刷新。`ssh esp-hub python3 /opt/esp-dash/ha_setup.py inspect` 只列设备与指标名，不列数值。
6. `./deploy.sh health` 绑定唯一一台包含健康传感器的手机。多台时使用 `./deploy.sh health --device <HA设备ID>`。根据官方传感器 unique ID 匹配，避免用全局 `sensor.weight` 猜测来源；不混入其他手机或心率指标。绑定会重启 HA；完成后在手机首页再次下拉刷新，等待首次实际上报。

绑定配置保存在 `/etc/esp-dash/health.json`，通过 systemd `LoadCredential` 提供给 hub；普通 `deploy.sh hub` 不覆盖已经绑定的设备。移除或更换手机后重新绑定。更换来源会开始独立的观察记录，避免把不同人的数据拼接在一起。

可接入的十项源指标为：步数、步行跑步距离、运动分钟、活动能量、总睡眠、核心睡眠、深度睡眠、REM 睡眠、体重、体脂。实体存在不代表苹果健康内一定有记录；未授权、源数据为空、手机锁屏尚未完成首次读取时可能显示 `unavailable`。App 更新及后台调度也影响实际更新时刻。手机 App 的连接测试应在实际设备上进行，服务器本机访问成功不能代表所有公网客户端均可用。

## 数据与历史

- hub 每 60 秒通过 HA 本地 `/api/template` 一次性查询配置中的实体；请求处理线程只返回缓存，不等待 HA。
- 手机传感器提供当前值、HA 状态上报时间，不提供 HealthKit 样本的测量日期和过去记录。界面显示“同步时间”和“未提供测量日期”，睡眠称为“最近睡眠”。
- 日活动按北京时间归档为实际收到的观察值。`kiki_health_reports` 只监听已绑定实体的官方 `mobile_app` 更新信号，记录时间戳，不存健康数值，也不恢复上次启动的时间戳。hub 仅接受与该凭证时间一致的 HA 状态；HA 重启恢复数值、实体重载或普通状态写入都不能冒充手机上报，相同数值的真实上报仍有效。历史不做补传或推算；首次接入前和漏同步日期留空。过了午夜，昨天的计数不会变成今天的计数，今天没有数据时显示 `--`。
- 相同体重重复上报不生成新记录；只有值变化才产生新的体重观察。每个观察日保留当日最后值。屏幕的跨度是已记录日期的实际跨度，最多 30 日。
- 私有状态是 `/var/lib/esp-dash/health.json`，文件权限 `0600`，原子替换。首次观察立即保存，之后最多每 300 秒落盘；突发断电可能丢失最近五分钟的更改。历史窗口保留最近 30 天及最新状态。
- 私有缓存版本为 2；旧版未验证手机上报的缓存可能包含误归档的记录，不再继承，首次升级后重新开始观察。设备接口的 `health.schema` 仍为 1。
- 绝对体重只留在 HA 与 hub。设备接口只输出相对 0.1 kg 数值，不包含体重基线；日志、诊断和资源观察不输出健康数值。设备不新增健康 NVS 存储。
- 所选健康实体从 HA Recorder 排除，清单在 HA 配置目录的 `health_recorder.yaml`。hub 的每日观察记录承担本项目历史存储，不重复写入高频 HA 历史。旧 HA 历史仍遵循原有清理策略。
- HA 读取失败保留最后一次成功数据；健康解析失败只标记健康缓存，不影响其他状态页、航班、日历及 OTA 健康确认。

## 设备接口

现有 `/api/device/status` 增加可选 `health`，仍使用同一设备令牌和 8 KiB 响应上限。健康对象自身最多 2 KiB。旧固件会忽略新字段，新固件连接旧 hub 时显示待接入。

响应接近上限时优先缩短较旧的 CPU 历史，再减少最旧的事件和运行记录，保留当前指标和最新事件／运行。UTF-8 名称按设备实际字节容量截断；不会因为新增健康摘要让整份状态超出设备缓冲。

`health.schema = 1`：

| 字段 | 含义 |
|---|---|
| `configured`, `ok` | 是否绑定来源、最近一次 HA 查询是否成功 |
| `fetched_at`, `since`, `day` | 最近成功读取、开始观察的 UTC 秒；当前北京时间日期的 epoch-day |
| `values` | `steps`, `distance_m`, `exercise_min`, `active_kcal`, `sleep_min`, `core_min`, `deep_min`, `rem_min`, `body_fat`；体脂单位 0.1%，其余按字段名；未知为 `null` |
| `source_at` | 与上述九个字段同序、已验证为手机实际上报的 HA `last_reported` UTC 秒；不是测量日期 |
| `activity` | 从旧到新的七行 `[步数, 运动分钟]`，缺失为 `null` |
| `weight_hist` | 从旧到新的三十日相对 0.1 kg 值，首个有效日为零，缺失为 `null` |
| `weight_delta`, `weight_change`, `weight_span`, `weight_at` | 距前一观察、窗口内变化、已记录跨度、最近体重观察时间；不冒充称重日期 |

板子的 C 模型固定容量，当前仅 272 字节。健康页使用一个绘制面和三个触摸区，以代码绘制卡片、文字和图表；没有额外图像缓冲或完整标签对象树。UI 的非 DMA 状态快照优先分配到 PSRAM，保留内部 RAM 给 LVGL、Wi-Fi 和任务栈。

## 诊断和验收

```sh
python3 -m unittest discover -s server -p 'test_*.py'
cmake -S sim -B build-sim -G Ninja && cmake --build build-sim
ctest --test-dir build-sim --output-on-failure
./esp.sh shots
./esp.sh build
./esp.sh flash                 # 唯一 USB 更新入口；保护 NVS 和已存在的 OTA 槽
./esp.sh demo health           # 合成数据，只在设备上演示
./esp.sh demo off
./esp.sh dash                  # 健康状态、读取年龄、模型大小，不输出数值
```

板子在原有状态请求的 `X-Dash-Heap-Free`、`X-Dash-Heap-Min`、`X-Dash-Uptime` 头附带内部堆和运行时间，不新增网络连接。`GET /api/device/diagnostics` 需要设备令牌，只返回这些资源计数及接收时间；旧固件没有计数时返回空对象。

`python3 /opt/esp-dash/health_watch.py --once` 在 hub 输出一次资源检查。使用 `--hours 24` 每五分钟采样到 `/var/lib/esp-dash/health_watch.jsonl`（`0600`）：HA CPU／内存／重启、hub 内存／重启、服务器可用内存、响应字节数、健康同步状态／条数／年龄、板子堆和运行时间。记录不包含健康值。最后的 `complete` 行说明观察窗口已结束；观察中、断网、设备离线或记录缺口均不能当作已通过 24 小时稳定性验收。
