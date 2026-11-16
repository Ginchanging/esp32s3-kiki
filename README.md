# ESP32-S3 桌面状态屏 · Kiki

ESP32-S3 N16R8 接入校园网（SYSU-SECURE-2.4G，WPA2-Enterprise PEAP）。屏幕首页住着一只原创像素橘猫 **Kiki**：它按作息过日子，你的 CI、服务器和 Muse 提醒一有动静，它就会做出反应。界面支持中英文切换，共 9 页：

| 页 | 内容 |
|---|---|
| Kiki 的房间 | 窗外的天空跟着真实时间变化，有像素时钟和日期；Kiki 在做什么、心情如何都一眼可见；有对话气泡，以及 4 个状态芯片（点击跳到对应页） |
| GitHub | ISBF-Lab/ISBFweb 最新一次运行的大色块、你的提交热力图（15 周）、最近两次运行 |
| 服务器 | 三台服务器各一条近 1 小时的 CPU 心电图，外加内存、磁盘和服务状态 |
| 番茄钟 | 25 分钟专注 + 5 分钟休息，环形进度条，记录今天完成的番茄数；LED 跟随番茄钟变色 |
| Kiki 的信箱 | 最近 5 条消息：Muse 提醒、AI 任务推送、米家提醒、Kiki 的日记 |
| 米家 | 台灯开关（点按钮控制）、洗衣机状态和预计几点洗好（洗完会提醒）、饮水机水温和滤芯 |
| 头顶航班 | 北向朝上的像素雷达，显示 20 公里内的航班；卡片显示航班号/呼号、参考航线、机型、高度和水平距离 |
| iCloud 日程 | 今天起七天的安排，含全天和跨日日程；点击墙历直达，点日程读完整标题 |
| 健康运动 | 苹果健康每日运动汇总、最近睡眠及阶段、体脂率和相对体重曲线；点运动／睡眠／体态切换 |

第八页是 iCloud 日程：显示今天起七天的安排，点房间墙上的小日历可以直达。

翻页：左右滑动。页面首尾相接：在首页手指从左往右划，会跳到最后一页（健康运动）；在最后一页往左划，会回到首页。

回首页：在任意页面的空白处快速点两下（点在按钮上不算）。

**Kiki 的作息**（`components/dash_model/dash_schedule.c`，按东八区时间计算，可以改）

| 日子 | 安排 |
|---|---|
| 工作日 | 7:00 晨练（跳绳）· 7:40 早餐 · 8:30 上课 · 11:50 午饭 · 13:00 午睡 · 14:00 写代码 · 17:40 晚饭 · 18:40 自由时间 · 22:50 洗漱 · 23:30 睡觉（夜间模式：画面变暗，LED 不亮） |
| 周末 | 9:00 起床晨练，14:00 晒太阳 |

自由时间里，Kiki 会坐着发呆、看窗外、趴着休息，每种姿势保持 1–3 分钟，偶尔眨眼、摆尾，穿插几秒钟的伸懒腰、舔爪和哈欠。看窗外采用背对镜头、朝窗户坐着的姿势，头部保持不动，只轻摆尾巴，姿势参考 [Octavi Navarro 的 Cat Cafe #01](https://x.com/OctaviNavarro/status/1093242223255322638)。毛线球每次只玩 8–12 秒，两次至少隔 10 分钟，21:30 后收起来。气泡跟随当前动作；切页、切语言或通知结束后按实际时间接着活动。洗漱改为低头舔爪，爪子连着橘色前臂，脸旁没有白块。

**Kiki 的反应**

| 触发 | 反应 |
|---|---|
| CI 成功 | 跳起来、撒彩纸，LED 绿闪；正在吃饭也会先跳完再接着吃 |
| CI 失败 | 炸毛，之后一直冒汗担心 |
| CI 开始运行 | 竖起耳朵 |
| 服务器掉线 | 惊慌失措 |
| 服务器恢复 | 冒出爱心 |
| Muse 提醒 | 举牌子并弹窗，LED 白闪 |
| 洗衣机洗完 | 跳起来，弹出“米家提醒”，LED 绿闪，消息进信箱 |
| 民航客机或货机进入 5 公里 | 首页临时切到随时间变色的露台，Kiki 保持背影静静坐着，只轻轻摆动尾巴；6 秒后恢复房间并弹出可点击航班卡，其他页直接弹卡片 |
| 点一下 Kiki | 喵一声，并轮流汇报 CI、服务器和下一项作息 |
| 长按 Kiki | 开始或暂停番茄钟 |

**Kiki 的日记**

- 每晚 23:00，Kiki 往信箱写一条今日总结，弹窗显示 15 秒，LED 不闪。周日这条换成周报。
- 日记内容：
  - 番茄数和专注时长
  - GitHub 提交次数
  - CI 成功和失败次数
  - 洗了几次衣服
  - 服务器掉线次数
- 周报还会写出最努力的一天；接入体脂秤时，还会写这一周的体重变化（体脂秤目前没接入，见“米家”一节）。
- 数字为 0 的项不写。结尾一句由 Kiki 按当天数据挑。
- 数据来源：
  - 番茄钟记录存在板子上（最近 7 天）。
  - 其余计数来自 hub。hub 把每天的计数存在 `/var/lib/esp-dash/ledger.json`，重启或重新部署都不会清零。
- 测试：`./esp.sh cat diary`（日记）、`./esp.sh cat diary week`（周报），或在模拟器里按 `y` / `Y`。手动触发不影响当晚 23:00 那一条。

**修改 Kiki 的样子**

1. 编辑 `scripts/cat_art.py`，用形状和像素画出图层。图层会写到 `assets/cat/*.txt`，这是可以直接手改的 ASCII 像素文件。
2. 运行 `python3 scripts/cat_art.py --sheet shots/cat_sheet.png`，生成设计稿方便查看。
3. 运行 `python3 scripts/gen_sprites.py`，生成 `components/dash_ui/sprites/dash_sprites.c`（4bpp，约 24 KB）。

屏幕已接入，也可以用两种方式在 Mac 上看界面：

| 命令 | 看到的是什么 |
|---|---|
| `./esp.sh mirror` | **板子的真实画面**：板子上 LVGL 实际渲染出的每一帧，经 USB 串口传到 Mac 窗口；数据是实时的。鼠标点击、拖动会作为触摸传回板子 |
| `./esp.sh sim` | **模拟器**：同一份界面代码在 Mac 上运行，数据来自 `fixtures/` 里的假数据，用来调界面、测各种状态 |

镜像窗口右侧是板子 LED 的实时颜色，下方是命令框，可以输入 `lang en`、`demo gh_failed`、`muse 文字`、`dash` 等。

镜像窗口里也能模拟旋钮：滚轮是拧，Shift + 滚轮是按住拧，右键是按下。

镜像需要注意：
- 串口同一时间只能被一个程序占用，开着镜像时 `./esp.sh dash` 这类命令会连不上，请改用窗口里的命令框。
- 只抓一帧截图、不开窗口：`./esp.sh mirror --shot out.ppm`。

```
校园网 ── ESP32-S3 ──HTTPS:8443(证书固定)──► hub  8.138.138.222 (esp-dash-hub)
                │                              ├─ 每 90 s 拉取 GitHub Actions
                │                              ◄── agent 34.68.253.253 (gcp-isbf)
                │                              ◄── agent 8.155.132.213 (aliyun-codex)
                │                              └─ 每 5 s 读本机 Home Assistant ◄─MQTT推送─ 小米云 ◄── 米家设备
                └──MQTT TLS:9503──► 巴法云 musealert ◄── Muse
```

## 目录

| 路径 | 说明 |
|---|---|
| `main/` | 固件主体：共享状态、串口命令、UI 任务、hub 拉取、Muse MQTT、LED |
| `components/campus_net/` | 由原 `campus.c` 改来：校园网认证和 `@sysu` 串口协议。NVS 布局不变 |
| `components/dash_model/` | 纯 C 数据模型，固件和模拟器共用：JSON 解析、数据过期判断、LED 规则、时间格式 |
| `components/dash_ui/` | 纯 LVGL 界面、中英文字符串表（`dash_i18n.c`）、字体 |
| `third_party/` | LVGL v9.6.0、cJSON v1.7.19，以及两边共用的 `lv_conf.h` |
| `sim/` | Mac 模拟器 |
| `fixtures/` | 模拟数据，模拟器和板子的 `demo` 命令共用 |
| `server/` | hub/agent 程序 `dashd.py`、systemd 单元、`deploy.sh`（`secrets/` 和 `certs/hub.key` 不入库）；`homeassistant/` 和 `mihome.json` 是米家接入 |
| `campus.py`、`esp.sh` | Mac 上的工具（Windows 上仍可用 `esp.ps1`） |

## 日常命令（Mac）

```sh
./esp.sh mirror              # 真板实时画面（点击 = 触摸）
./esp.sh sim                 # 模拟器窗口（假数据）
./esp.sh shots               # 生成全部截图到 shots/（含 sheet_*.png 总览）
./esp.sh build               # 编译固件（ESP-IDF v6.1，位于 ~/esp/esp-idf-v6.1）
./esp.sh flash               # 烧录；campus_nvs 里的 NetID、令牌、UID 都会保留
./esp.sh ota                 # 编译、发布并等待 Wi-Fi 更新结果，不需要 USB
./esp.sh ota status          # 通过 Hub 查询固件槽位、版本与更新结果
./esp.sh ota cancel          # 撤销尚未开始的更新；已经下载则等待结果
./esp.sh ota rollback        # 重新发布上一份确认可用的固件
./esp.sh status              # 校园网状态
./esp.sh dash                # 仪表盘状态：令牌、MQTT、上次拉取、内存
./esp.sh monitor             # 串口日志，Muse 提醒会自动解码成中文
./esp.sh lang en|zh          # 切换语言并保存（也可以点屏幕右上角的按钮）
./esp.sh demo gh_failed      # 演示数据：ok / gh_failed / gh_running / server_down / off
./esp.sh muse "文字"          # 在板子上显示一条本地测试提醒
./esp.sh led 48|38|off       # 板载 RGB 灯引脚（DevKitC-1 v1.0 为 48，v1.1 为 38）
./esp.sh cat act 4           # 强制 Kiki 进入某个作息（0 睡觉 … 10 晒太阳），测试用
./esp.sh cat react 1         # 触发反应：1 CI 成功 2 失败 3 开始 4 服务器掉线 5 恢复 7 摸头 8 早安
./esp.sh cat night on        # 夜间模式；cat time 3 = 作息时间快进 3 小时；cat auto = 恢复自动
./esp.sh mi lamp on          # 米家：开/关台灯（on / off / bri 60），和点第 6 页的按钮一样
./esp.sh cat diary [week]    # 立刻写一条 Kiki 日记（加 week 是周报）
./esp.sh cat flight          # 播放飞越演示，弹窗明确标注“飞越演示”
./esp.sh demo flight         # 航班测试数据；demo off 恢复真实数据
./esp.sh knob cw 2           # 模拟旋钮：cw / ccw [格数]、hold cw 4、press、long；rev = 反转方向
```

**模拟器按键**

| 按键 | 作用 |
|---|---|
| `s` `f` `x` | CI 成功 / 失败 / 开始 |
| `d` `u` | 服务器掉线 / 恢复 |
| `m` | Muse 提醒 |
| `p` | 番茄钟 |
| `t` | 作息时间 +1 小时 |
| `c` | 下一个作息 |
| `n` | 夜间模式 |
| `w` | 米家：洗衣机洗完 |
| `v` | 飞越演示（命令行截图可用 --flyover --ms 2400） |
| `y` `Y` | Kiki 的日记 / 周报 |
| `[` `]` | 旋钮：拧（翻页） |
| `{` `}` | 旋钮：按住拧（台灯亮度） |
| `k` `K` | 旋钮：短按（台灯开关）/ 长按（番茄钟） |
| `0` | 恢复自动 |
| `l` | 中英文切换 |
| `1`–`8` | 切换 fixture |
| 点台灯按钮 | 米家开关（模拟器里 0.7 秒后生效） |
| `←` `→` | 翻页 |

鼠标的用法和触摸一样：点 Kiki 是摸头，长按 Kiki 是番茄钟，拖动是翻页。旁边的小窗口是虚拟 LED。

`./esp.sh shots` 会把总览图输出到 `shots/sheet_*.png`：

| 总览图 | 内容 |
|---|---|
| `sheet_pages_zh` / `sheet_pages_en` | 8 个页面（中文 / 英文） |
| `sheet_routine` | 一天的作息 |
| `sheet_idle_zh` / `sheet_idle_en` | 自由时间的 7 种动作和对应台词 |
| `sheet_reactions` | 各种反应 |
| `sheet_moods` | 各种心情，含夜间、离线、开机 |
| `sheet_details` | 细节页 |
| `sheet_flights` | 飞越演出、夜晚、提醒卡及航班空数据/过期/断线状态 |
| `sheet_calendar` | 日历空日程、缓存和未接入状态 |
| `sheet_holidays` | 11 种节日布景的白天和夜晚 |

`shots/kiki_idle_pixels.png` 是新姿势的原始像素与 4 倍预览；`kiki_groom.gif` 等展示连续帧，`idle_timeline.csv` 记录一整晚的动作切换。运行 `ctest --test-dir build-sim --output-on-failure` 可验证作息、轮换规则和动画恢复。

**字体**：在 `dash_i18n.c` 里新增中文后，要重新运行 `./scripts/gen_fonts.sh`。16px 字体包含 GB2312 一级的 3755 个常用字，以及界面里出现的所有汉字。

## Muse 提醒

1. 在巴法云控制台进入“MQTT 设备云”，创建主题 `musealert`。
2. 在自己的终端运行 `./esp.sh bemfa`，按提示粘贴私钥 UID。输入不会显示，UID 只存在板子上，不要发到聊天里。
3. 测试：运行 `BEMFA_UID=... ./esp.sh muse-send "测试提醒"`；不设环境变量时，会提示你输入 UID。

板子通过 TLS 连接 `bemfa.com:9503`，客户端 ID 就是 UID，订阅主题 `musealert`。巴法云在重新订阅时会重发上一条消息，板子会按内容哈希识别并忽略，所以重启后不会重复提醒。

## AI 任务推送（Claude Code / Codex）

板子同时订阅巴法云主题 `agentalert`，接收通用包 [agent-bemfa-notify](https://github.com/Ginchanging/agent-bemfa-notify) 发来的推送。

| 推送类型 | Kiki 的反应 | 弹窗与灯效 |
|---|---|---|
| 长任务完成 | 跳起来 | 绿色弹窗“Claude Code 完成了”，LED 绿闪 |
| 需要你确认 | 竖耳朵，头上冒“!” | 黄色弹窗，LED 黄闪 |
| 出错 | 炸毛 | 红色弹窗，LED 红闪 |

- 所有推送都会进“Kiki 的信箱”，并标上来源和项目名。
- 夜间不亮灯。
- 本地测试：`./esp.sh agent done 测试文字`（也可以把 `done` 换成 `attention` 或 `error`）。
- 模拟器里按 `a`、`b`、`e` 也能触发这三种推送。

## 米家（洗衣机提醒、台灯开关）

米家没有给个人用的开放 API。唯一官方的接入方式，是小米自己出的 Home Assistant 集成 [Xiaomi Home](https://github.com/XiaoMi/ha_xiaomi_home)：它用小米官方 OAuth 登录，设备状态由小米云实时推送。

所以这里在 hub 上用 Docker 跑一个 Home Assistant，只监听 `127.0.0.1:8123`，不对外开放，内存上限 700 MB。`dashd` 每 5 秒通过 HA 的本地 REST API 读一次设备，板子再从 hub 取数据：

- 状态随每 30 秒一次的拉取更新。
- 点开关时，板子会立刻发 `POST /api/device/mi`。
- 板子本身不接触小米账号，也不接触米家协议。

**首次设置**（只需做一次）

1. 部署 HA 和集成：`cd server && ./deploy.sh ha`。镜像版本和集成版本都是固定的。全新安装先使用 Docker 桥接网络，仅把端口发布到 hub 的 `127.0.0.1:8123`，用于创建账号。
2. 打开隧道：`ssh -N -L 8123:127.0.0.1:8123 esp-hub`，然后浏览器打开 http://localhost:8123，创建 HA 账号。
3. 让 Mac 认得 `homeassistant.local`：另开一个终端运行 `dns-sd -P KikiHA _http._tcp local 8123 homeassistant.local 127.0.0.1`，登录完成后按 Ctrl-C 关掉。
   - 原因：小米授权完成后，会把浏览器跳到固定地址 `homeassistant.local:8123`。不运行这一步，Safari 会一直卡在授权页。
4. 添加集成：依次点 设置 → 设备与服务 → 添加集成 → Xiaomi Home，区域选「中国大陆」，然后登录小米账号并确认授权。授权后会自动跳回 HA，选择家庭和设备即可。
5. 生成令牌：点左下角用户名 → 安全 → 长期访问令牌 → 创建，然后在仓库根目录运行 `./server/set_ha_token.sh`，粘贴令牌（输入时不显示）。再运行 `cd server && ./deploy.sh ha`，通过管理员 API 确认回环监听后切换到主机网络；验证失败会保留隔离的引导容器。
6. 配置设备：在 `server/mihome.json` 里填上设备的实体名（HA 的「开发者工具 → 状态」里能查到），再运行 `./deploy.sh hub`。

**饮水机和体脂秤**

- 饮水机卡片显示：工作状态和出水温度、两个滤芯的剩余寿命（低于 20% 变黄，低于 10% 变红）、出水 TDS。
- 体脂秤（米家体脂秤 S400）目前**没有接入**：
  - 它在 Home Assistant 里没有体重读数，只有“实时体重上报”等事件。
  - 2026-11-14 实测：称重记录进了米家 App（经手机蓝牙同步），但没有作为事件推给 Home Assistant，所以卡片已从 `mihome.json` 撤掉。
  - hub 和固件里的支持代码还在（`kind: "scale"`，只显示体重变化，具体体重只存在 hub 的 `/var/lib/esp-dash/scale.json`）。以后有能转发的蓝牙网关，或改成板子直接收秤的蓝牙广播时，可以再接回来。

**说明**

- 洗衣机卡片显示“预计 HH:MM 洗好”，不显示倒计时。这台洗衣机运行中大部分时间不上报剩余时间（2026-11-14 实测：报到 71 分钟后停了 80 多分钟没更新），所以 hub 用“最后一次上报的时刻 + 当时的剩余分钟”算出完成时刻发给板子。超过预计时间还没洗完时显示“马上洗好”，暂停时不显示。
- 洗衣机从“运行中”变成“完成 / 空闲”，并且剩余时间为 0 时，hub 会发出一个 `mi_done` 事件。板子收到后，Kiki 会跳起来，消息进信箱，LED 绿闪。
- 改了 `mihome.json` 里的中文设备名之后，要重新运行 `./scripts/gen_fonts.sh` 并重新烧录，小字号字体才会包含这些字。
- 米家云或 HA 出问题时，第 6 页右上角会显示「连接中断」，不影响其他页面。

## 苹果健康与每日运动

第九页有「运动／睡眠／体态」三个标签。运动显示今日步数、步行跑步距离、运动时间和活动能量，下方是七日同步记录；睡眠显示最近一次睡眠及核心、深度、REM 时长；体态显示体脂率、距上次记录和记录以来的体重变化，以及最多三十日的相对曲线。

数据通过 iPhone 的官方 Home Assistant App 读取苹果健康，再发到 `https://ha.kiki-home.top`。hub 每分钟批量读取一次，板子沿用原来的三十秒状态请求。手机后台同步由 iOS 调度，锁屏时可能延后；缺失或未授权的数据显示 `--`，不当作零。官方 App 提供当前值，测量日期未知时明确标注；接入前的历史和逐次训练记录不在首版范围内。

首次设置、数据格式、诊断和三十日保留规则见 [server/HEALTH.md](server/HEALTH.md)。常用命令：

```sh
cd server && ./deploy.sh ha             # 手机集成、回环监听与可信代理；保留已有 HA 账户
cd server && ./deploy.sh ha-proxy       # Caddy 公共 HTTPS 入口；先设置域名 DNS
cd server && ./deploy.sh health         # 手机授权后绑定唯一一台含健康传感器的 iPhone
./esp.sh demo health                   # 板上合成数据演示，绝不写入真实健康记录
./esp.sh demo off                      # 恢复实时数据
./esp.sh dash                          # 健康读取状态与堆内存；不输出健康数值
```

绝对体重只留在服务器，设备、截图和设备接口只接收变化量。hub 的同步记录保留最近三十天，文件权限 `0600`；新增健康实体从 HA Recorder 排除，避免重复保存高频历史。健康同步失败不影响服务器、航班、日历和 OTA 健康确认。

## iCloud 日历与节日布景

房间墙历显示当天日期，有日程时带一个小标记。点墙历进入第八页，在七天日期条上选择日期；列表显示全天事件、时间、标题和来源日历。点一条日程可读完整标题，再轻点返回。每天超过八条时用底部按钮翻组，列表可上下滚动。首版只读，不修改苹果日历，也不额外弹出日程提醒。

接入复用 hub 上的 Home Assistant：

1. 建立隧道：`ssh -N -L 8123:127.0.0.1:8123 esp-hub`，浏览器打开 `http://127.0.0.1:8123`。
2. 在 [Apple 账户](https://account.apple.com/)的「登录和安全性 → App 专用密码」生成一个密码，名称例如 `Kiki 日历`；需要开启双重认证。
3. HA「设置 → 设备与服务 → 添加集成 → CalDAV」，地址填 `https://caldav.icloud.com/`，用户名填 Apple 账户邮箱，密码填刚生成的 App 专用密码，保留 SSL 验证。通过界面添加才能包括全天事件。
4. 将该连接重命名为 **Kiki iCloud**。`server/calendar.json` 的 `entry_title` 按这个名称识别连接，汇总其全部可读取日历，不混入其他 HA 日历账户。新增 iCloud 日历后，重新加载该 CalDAV 集成，让 HA 发现新日历。
5. `cd server && ./deploy.sh hub`。hub 复用现有 HA 令牌；Apple 密码只保存在 HA 配置中，不进入仓库、聊天或固件。

hub 每 15 分钟更新今天起七天的窗口；设备进入日程页、切日期或翻组立即取缓存，平时每分钟检查一次。同步失败保留上次成功数据并标记；缓存只在内存中，重启后重新取数。`/api/device/calendar?day=YYYY-MM-DD&offset=0` 使用现有设备令牌，每次最多八条、回复不超过 8 KiB。`./esp.sh dash` 的 calendar 行提供同步状态、条数和数据年龄，不输出日程正文。

节日布景由设备按北京时间独立判断，无需在 iCloud 建节日日程，也能在断网时工作：

| 日期 | 布景 |
|---|---|
| 农历正月初一至初七 | 春节灯笼、春联和红包 |
| 农历正月十五 | 元宵兔子花灯、汤圆 |
| 农历五月初五 | 端午粽子、艾草 |
| 农历七月初七 | 七夕双星、鹊桥 |
| 农历九月初九 | 重阳菊花、远山 |
| 冬至当天 | 窗外飘雪 |
| 公历 10 月 1–7 日 | 国庆小旗、夜间烟花 |
| 公历 10 月 31 日 | 万圣节南瓜灯、蝙蝠 |
| 11 月第四个星期四 | 美国感恩节枫叶、丰收篮 |
| 公历 12 月 25 日 | 圣诞树、礼物 |
| 公历 8 月 7 日 | 生日蜡烛蛋糕 |

其余日子自动恢复普通房间，闰月不重复过节；重合节日按装饰层叠加，中央摆件优先生日。夜间沿用整体变暗，重要通知和飞越场景照常工作。

农历和冬至使用 [香港天文台公历农历对照表](https://www.hko.gov.hk/en/gts/time/conversion.htm)的离线数据，覆盖 2026–2099 年；公历节日和生日不受这个年份范围限制。数据来源和原表 SHA-256 保存在 `assets/calendar/holidays_2026_2099.json`，运行 `python3 scripts/gen_holidays.py` 可离线重建 C 表，只有 `--refresh-hko` 才会重新下载。没有直接使用 LVGL 的节气表，因为其部分年份的冬至日期不一致。

日历 fixture 为 `16_calendar.json` 至 `19_calendar_disabled.json`；运行 `./esp.sh shots` 查看中英页面和全部昼夜节日布景。验证命令：`python3 -m unittest discover -s server -p 'test_*.py'` 与 `ctest --test-dir build-sim --output-on-failure`。

## 服务器

**头顶航班**

- 位置数据来自 [adsb.fi](https://adsb.fi/) 的[公开 API](https://github.com/adsbfi/opendata)，用于个人非商业桌面屏；hub 每 10 秒查询一次，覆盖情况取决于接收站。机型、位置和航线字段可能缺失，空雷达不代表实际空域一定没有飞机。
- 参考航线由 [adsbdb](https://www.adsbdb.com/) 补全；它不是实时飞行计划。补全失败显示“航线待补全”，航向和位置继续更新。字母数字混合的 ATC 呼号保留为呼号，不冒充公众航班号。
- 在 `server/secrets/flights.json` 设置固定位置，格式见 `server/flights.example.json`。`coord_type` 必须明确为 `wgs84`、`gcj02` 或 `bd09`；百度拾取坐标用 `bd09`，hub 近似换算到 WGS84。文件不入库，部署时作为 systemd credential 传入。
- 雷达半径默认 20 公里，提醒半径默认 5 公里，均按地面水平距离。仅新鲜的、未落地且可识别的民航运输目标提醒；已标记军机、直升机及地面目标排除。身份不确定的目标仅显示在雷达上。
- 同一架飞机一次经过只提醒一次。再次提醒需要离开 7 公里至少 2 分钟，且距上次提醒超过 30 分钟。断网不算离开；hub 和板子重启后建立基线，不重播旧提醒。飞越不闪灯，夜间沿用变暗规则，番茄钟持续计时，重要通知优先。
- `/api/device/flights` 使用原有设备令牌，回复不超过 8 KiB；它的失败和过期状态独立于原仪表盘。板子每 10 秒拉取，原状态仍每 30 秒拉取。日常 `./esp.sh dash` 可查看航班状态与数据年龄。
- 后端校验：`python3 -m unittest discover -s server -p 'test_*.py'`；共享 C 模型校验：`ctest --test-dir build-sim --output-on-failure`。`fixtures/9_flight.json` 至 `15_flights_disabled.json` 覆盖正常、多机、无航班、未知字段、过期、断线和未配置。

坐标换算公式改编自 MIT 许可的 [wandergis/coordtransform](https://github.com/wandergis/coordtransform)，许可见 `server/flight_coord_LICENSE.txt`。

```sh
cd server && ./deploy.sh all     # 也可以单独部署：hub / gcp / codex
```

部署只会新增这些东西：

- `/opt/esp-dash`
- `/etc/esp-dash`（权限 root 600）
- `/var/lib/esp-dash`（仅 hub：Kiki 日记的每日计数、体重历史）
- 一个 systemd 单元，以 `DynamicUser` 运行，密钥通过 `LoadCredential` 传入

其他要点：

- 阿里云安全组需要放行 8.138.138.222 的 TCP 8443。
- 板子用的设备令牌在 `server/secrets/device_token`，用 `./esp.sh token` 写入板子。
- hub 证书是自签的（CN=esp-dash.local，有效期到 2036 年），已经固定在固件 `main/certs/hub.pem` 和各台 agent 上。

## 屏幕与外设接线

| 模块 | 引脚 → ESP32-S3 |
|---|---|
| 2.8" ILI9341V 屏 | VCC→**3V3**、GND→GND、SCK→12、MOSI(SDI)→11、CS→10、DC(RS)→9、RST→8、BL(LED)→7；MISO(SDO) 不用接 |
| FT6336G 触摸 | SDA→15、SCL→16、INT→17、RST→18 |
| SHT30 | 与触摸共用 SDA 15 / SCL 16，地址 0x44 |
| KY-016 RGB | R→4、G→5、B→6、GND→GND |
| EC11 旋钮 | CLK→1、DT→2、SW→42、+→**3V3**、GND→GND（都在右排针，挨着 GND） |

**EC11 旋钮**
- 固件已经支持旋钮，没接时不会有任何反应，所以现在就能烧录。
- `+` 一定要接 3V3，不要接 5V。ESP32-S3 的引脚不耐 5V。
- 操作：

  | 动作 | 作用 |
  |---|---|
  | 拧 | 翻页，首尾相接 |
  | 短按 | 台灯开/关 |
  | 按住再拧 | 调台灯亮度，每格 5%，屏幕底部显示“台灯 60%”；松手或停 0.4 秒后发出 |
  | 长按（不拧） | 开始/暂停番茄钟 |

- 夜间屏幕变暗时，动一下旋钮会先点亮屏幕。
- 拧的方向反了：运行 `./esp.sh knob rev`。

- 接线以模块上的丝印为准。
- GPIO35–37 被 Octal PSRAM 占用，19/20 是 USB，这些脚都不要用。

**屏幕供电**
- 这块开发板的 `5Vin` 脚默认**只进不出**，接在上面屏幕不亮。所以屏幕 VCC 接 3V3。
- 想改用 5V：用焊锡短接板子上的 `IN-OUT` 焊盘（RGB 灯旁边），5Vin 才会对外输出 5V。短接后，插着 USB 时不要再从 5Vin 接别的电源。

**屏幕调整**（`./esp.sh lcd`，改完立即生效，存在板子里）

| 现象 | 命令 |
|---|---|
| 看状态（是否识别到屏幕/触摸、上次触摸坐标） | `./esp.sh lcd` |
| 颜色像底片（黑白颠倒） | `./esp.sh lcd inv` |
| 红蓝互换（Kiki 变蓝） | `./esp.sh lcd rgb` |
| 画面左右镜像 / 上下颠倒 | `./esp.sh lcd mx` / `./esp.sh lcd my` |
| 点的位置左右反 / 上下反 / 横竖反 | `./esp.sh lcd tmx` / `tmy` / `txy` |
| 背光亮度 | `./esp.sh lcd bl 60`（0–100，BL 要接在 GPIO7 上才有效） |
| 花屏、条纹（线太长） | `./esp.sh lcd mhz 20`（默认 40） |
| 全部恢复默认 | `./esp.sh lcd reset` |
| 背光一跳一跳（3V3 供电时） | Wi-Fi 省电默认已关；`./esp.sh wifips on` 可重新打开做对比 |

同一个命令再执行一次就是关掉。`./esp.sh mirror` 照常可用，屏幕和 Mac 窗口同时显示，两边都能点。

## 校园网（原有功能，保持不变）

**认证**
- 认证方式是校方公开的 PEAP / MSCHAPv2，认证服务器名为 `sam.sysu.edu.cn`。
- 固件会校验证书链、有效期和服务器名称。
- NetID 和密码保存在未加密的 `campus_nvs` 分区。

**命令**

| 命令 | 作用 |
|---|---|
| `./esp.sh setup` | 输入 NetID 和密码（密码隐藏输入） |
| `./esp.sh scan` | 重新搜网 |
| `./esp.sh retry` | 用电脑时间校准后重试 |
| `./esp.sh clear` | 清除 NetID 和密码 |

**断线重连与校时**
- 断线后会自动重连，重试间隔从 15 秒逐步拉长，最多 10 分钟。
- 如果认证被拒（原因码 23），改为每 30 分钟重试一次，避免反复用错误密码请求认证服务器。
- 联网后通过 SNTP（ntp.aliyun.com）校时，并把时间存进 NVS。

**断电测试**：把数据线从电脑拔下，改插 USB 充电器，等 90 秒后再插回电脑，运行 `./esp.sh status`，看“独立供电”那条记录。

**串口注意（2）**：同一时间只能有一个程序打开串口。如果两个程序抢串口，或者监视程序被强杀，都可能触发一次 USB 复位（复位原因 11）。这不是程序崩溃。

**串口注意**：macOS 打开串口时，如果先拉低 DTR、而 RTS 仍是高电平，ESP32-S3 会复位。`campus.py` 打开串口时保持 DTR 为高，只拉低 RTS，这样执行命令不会让板子重启。

**固件更新与恢复**：16 MiB Flash 使用 `dual6-v1` 布局，两块各 6 MiB 的应用槽；配置分区的位置和大小不变。首次运行 `./esp.sh flash` 会先校验并保存当前完整 Flash 快照，再迁移到新布局。以后优先使用 `./esp.sh ota`：电脑通过已有 SSH 别名 `esp-hub` 发布，板子每 30 秒主动检查，沿用 Hub 固定证书和设备令牌下载。屏幕显示进度和 Kiki 的反应，校验成功后显示 3、2、1 再重启。新版通过存储、UI 心跳和 Hub 获取检查后确认；120 秒内未通过则回退，失败任务不会反复自动安装。相同版本不会重复重启。

USB 仍使用 `./esp.sh flash`，它会选择非活动槽并保留配置；无法确定启动槽时可用 `./esp.sh flash --recover` 恢复到 `ota_0`，保留配置和另一槽。迁移前的老固件不能直接写到 0x10000 当作回滚；它不支持新布局和 OTA 确认机制。不要用 `.flash-backup/` 整片写回做日常恢复，否则会覆盖保存的配置。

`./esp.sh ota` 默认等待 10 分钟；超时仅表示结果待确认，继续用 `ota status` 查询。OTA 只更新应用，启动程序或分区表变更仍需 USB。中英文更新画面可用 `python3 scripts/ota_shots.py` 生成；模拟器支持 `--ota downloading|verifying|countdown:3|validating|success|failed|rollback`。

[中大接入指南](https://inc.sysu.edu.cn/cat/156) · [乐鑫证书包文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/protocols/esp_crt_bundle.html) · [巴法云 MQTT 文档](https://cloud.bemfa.com/docs/src/mqtt.html)
