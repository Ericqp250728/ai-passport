<p align="right">
  <strong>简体中文</strong> · <a href="notification-mirror.md">English</a>
</p>

# 通知镜像（Notification Mirror）

通知镜像通过蓝牙低功耗上的 Apple 通知中心服务（ANCS），把 iPhone 的通知显示在
AI Passport 上。地图 App 的转弯提示会显示成带方向箭头的大字卡片，走路时不用一直
举着手机看路。微信、LINE、短信等其他 App 的消息会进入列表。它是为日本旅行做的：
字库覆盖常用中文、平假名、片假名，以及「駅」这类常用日本汉字。

![电脑上渲染的五个画面：导航、消息列表、消息详情、新消息横幅、配对引导](../assets/images/notification-mirror-preview.png)

截图由 `tools/mirror_preview.py` 生成：它在电脑上用虚构的通知运行真实的
`main/mirror_ui.c`。截图不代表设备上的实际显示效果，上机检查还没做。

## 和 iPhone 配对（只需一次）

1. 烧录固件后开机，设备会先显示"连接"页。
2. 在 iPhone 上打开 设置 → 蓝牙，在"其他设备"里点 **AI Passport**。
3. 点 **配对**。iOS 询问是否允许 AI Passport 显示通知时，选允许。
4. "连接"页变绿并显示"已连接"。之后只要两边都开着、距离不远，iPhone 会自动重连。

如果"连接"页提示缺少通知权限：打开 设置 → 蓝牙 → AI Passport 右侧的 (i)，
打开 **共享系统通知**，然后断开再重新连接一次。

想换一台手机配对：在"连接"页连续长按两次 ▼ 清除配对，再在旧 iPhone 上对
AI Passport 选择 **忽略此设备**。

## 页面与按键

| 按键 | 导航 | 消息 | 详情 | 连接 |
| --- | --- | --- | --- | --- |
| OK 短按 | 下一页 | 下一页 | 返回消息列表 | 下一页 |
| OK 长按 | — | 打开选中的消息 | — | — |
| 上 / 下 短按 | — | 移动选中项 | 上一页 / 下一页 | — |
| 下 长按 | — | — | — | 第一次进入待确认，3 秒内再按一次清除配对 |

- 收到导航更新时自动切到导航页并亮屏 15 秒；正在看消息详情时不打断。
- 其他新通知会在顶部弹出 5 秒横幅，来电横幅是琥珀色。
- 按键后屏幕先亮 20 秒，之后调暗，到 60 秒熄屏；熄屏时按下的第一下只负责点亮。
- 刚连上时，iPhone 会补发通知中心里已有的通知，它们会静默加入消息列表，不弹横幅。
- 表情符号会在显示前去掉；字库外的字会显示成方框，比如生僻汉字和韩文。

## 转弯提示一定能显示吗

镜像只能显示地图 App 以 iOS 通知形式发出的内容。谷歌地图或苹果地图会不会为每次
转弯发通知，取决于 App 本身、它的版本和通知设置；锁屏上的"实时活动"不属于 ANCS
通知。**出发前请先锁屏走一段短路线，确认转弯提示能显示在设备上。** 普通消息不受
这一点影响。

方向箭头是根据提示里的中文、日文或英文关键词推断的；文字里没有明确方向时就不画箭头。

## 不装开发环境直接烧录

本分支校验过的整片镜像可以用浏览器写入：

1. 用支持数据传输的 USB-C 线把设备连到电脑。
2. 在 Chrome 或 Edge 中打开乐鑫官方网页烧录工具 <https://espressif.github.io/esptool-js/>。
3. 点 **Connect**，选择 USB JTAG/serial 端口。
4. 地址填 `0x0`，选择 `FoloToy-AI-Passport-full.bin`，点 **Program**。不要点 **Erase Flash**。
5. 完成后拔掉数据线或按复位键重启。

写入整片镜像会替换当前固件，设备里保存的数据可能被重置。参见
[烧录与数据策略](development/engineering/firmware-layout.zh_CN.md#烧录与已存数据)。

## 文件

| 路径 | 作用 |
| --- | --- |
| `main/ancs_proto.c` | 纯逻辑：ANCS 事件解析、属性请求、Data Source 流式拼包、待取 UID 队列（有主机测试） |
| `main/notif_model.c` | 纯逻辑：通知列表、App 名称、去表情、"几分钟前"、箭头推断（有主机测试） |
| `main/mirror_nav.c` | 纯逻辑：页面、按键与背光状态机（有主机测试） |
| `main/mirror_ble.c` | NimBLE 外设 + ANCS GATT 客户端：广播、绑定、发现、订阅 |
| `main/mirror_ui.c` | LVGL 界面；只能在持有 `bsp_lvgl_lock()` 时调用 |
| `main/main.c` | 启动流程，以及独占通知列表和界面的应用任务 |
| `assets/fonts/mirror_font_*.c` | 生成的思源黑体字库：16 px 常规体 4 bpp、24 px 粗体 2 bpp，各约 5600 字 |
| `tools/gen_mirror_fonts.py` | 字库生成与覆盖检查 |
| `tools/mirror_preview.py` | 在电脑上渲染界面 |

基线的硬件测试页面（`main/demo_*.c`）为了保留主机测试仍在仓库里，但不再编进本固件。

## 配置与资源预算

- 必须设置 `CONFIG_BT_NIMBLE_ROLE_CENTRAL=y`，因为 NimBLE 的 GATT 客户端依赖它；
  `CONFIG_BT_NIMBLE_NVS_PERSIST=y` 让配对在重启后保留。LVGL 内存池由 24 KB 调到 32 KB。
- 应用镜像约 2.5 MB，大头是两个字库；8 MB 应用分区约 70% 空闲。静态 DRAM 约用 149 KB / 321 KB。
- 主机预览报告的 LVGL 池占用最高为 70%（消息列表页，64 位指针），设备上会更低。
- 等待手机连接时，广播间隔用 Apple 推荐的 152.5–211.25 ms。

## 验证状态

| 检查 | 状态 |
| --- | --- |
| ANCS 解析、通知模型、导航与字库覆盖的主机测试 | 由 `./tools/validate.sh --static` 运行 |
| 固件编译、链接与整片镜像检查 | 在无法访问乐鑫组件仓库的沙箱中完成。LVGL 9.5.0、esp_lvgl_port 2.9.0 和 button 4.2.1 改从 GitHub 获取，esp_codec_dev 是 1.5.2 加仅供编译的垫片；本固件从不初始化音频。请在本地重新运行 `./tools/validate.sh`。 |
| 页面布局 | 已在主机渲染图上检查 |
| 设备 | 尚未测试：与真实 iPhone 配对、重启后重连、通知送达、地图转弯提示、屏幕显示、续航 |
