# Kiki 洗漱与自由时间轮换验证

2026-11-15，本机模拟器和 ESP32-S3 N16R8 真板。

## 最终行为

- 洗漱和短暂理毛都使用低头舔爪：橘色前臂连接圆爪，只有两个浅色趾尖像素，移除了脸旁的浅色块。
- 自由时间的主要姿势为坐着发呆、背对镜头看窗外、趴着休息，每段 60–180 秒；短暂穿插伸懒腰、舔爪、哈欠和毛线球。
- 毛线球每次 8–12 秒，开始时间至少间隔 600 秒，21:30 后不出现。日常作息表和原有活动 ID 不变。
- 轮换由东八区日期和时间确定；切页、切语言、通知打断、番茄钟结束后按当时的时间恢复。没有有效时钟时只安静坐着。
- 看窗外的初版斜视表情已按用户反馈删除。最终采用背影，头部固定，只有尾巴轻摆。姿势参考：[Octavi Navarro — Cat Cafe #01](https://x.com/OctaviNavarro/status/1093242223255322638)。同时查看了 [Elthen 的猫动画](https://elthen.itch.io/2d-pixel-art-cat-sprites)作为侧身姿势参考。Kiki 的像素仍由 `scripts/cat_art.py` 生成。

## 检查与结果

- `python3 scripts/cat_art.py --sheet shots/cat_sheet.png`、`python3 scripts/gen_sprites.py`：63 个图层、47 个 look、15 色。与修改前备份逐文件比较，原有像素图层只有 `body_wash.txt` 改变；写代码、睡觉和露台背影图层保持原样，原有 37 个 look 的名字、顺序保留。
- `./scripts/gen_fonts.sh`：为新增中文台词重新生成字体。英文看窗外台词缩短为 `Looking outside~`，240 像素宽界面上可完整显示。
- `cmake --build build-sim`、`ctest --test-dir build-sim --output-on-failure`：5 项全部通过，包括航班模型、日历模型、日历操作、自由时间轮换和实际 LVGL 猫动画。
- `idle_timeline`：连续 14 晚 18:40–22:50 逐秒检查，98% 时间为安静的主要姿势，毛线共出现 72 次；检查动作时长、间隔、21:30 截止、无连续同类主要姿势、时钟前后跳转及原有洗漱/睡觉边界。另有整日采样。
- `idle_animation`：直接比较 LVGL 渲染像素，检查眨眼/摆尾的时间位置、画面重建后续接、通知结束恢复、番茄钟/睡觉优先级、无时钟回退和原有 `CAT look fish/wash/auto` 名称。
- `./esp.sh shots`：重新生成全部页面、作息、通知、节日和待机动作总览；检查了中英文台词、原始像素和 4 倍连续帧，以及低头/趴卧/伸展时的心情表情位置。
- `./esp.sh build`：成功，最终镜像 1,931,008 字节，2 MiB 应用分区剩余 166,144 字节（8%）。
- `./esp.sh flash`：通过仓库脚本烧录，写入哈希校验成功，保留 NVS。
- 最终镜像 SHA-256：`03e8418dccd3ffa6df1d7e076518a644fd96e34992e56feeb48f2d99216ad1a2`。
- 真板已验证低头舔爪和背影看窗外的画面及台词。背影画面使用自由时间和临时时间偏移，让真实选择器选中该动作；随后已执行 `CAT auto` 恢复真实作息。原来的英文语言设置保持。
- 最终版本真板观测：Hub 实时、MQTT 已连接、米家/航班/日历正常，连续获取失败 0 次；摆尾截屏坏帧 0，内部堆最低 15,164 字节，空闲时约 29 KiB。

## 本机预览

- `shots/sheet_idle_zh.png`、`shots/sheet_idle_en.png`：真实选择器选出的七种动作和台词。
- `shots/kiki_idle_pixels.png`：原始像素及 4 倍预览。
- `shots/kiki_groom.gif`、`shots/kiki_window.gif`、`shots/kiki_stretch.gif`、`shots/kiki_yarn.gif`：连续帧。
- `shots/idle_timeline.csv`：2026-11-13 整晚动作切换，203 行，没有 21:30 之后的毛线动作。
- `shots/board_kiki_groom.png`：真板舔爪；`shots/board_kiki_window.png`：最终版本真板看窗外。

`shots/` 是可重新生成的本机产物，按项目约定不纳入 Git。
