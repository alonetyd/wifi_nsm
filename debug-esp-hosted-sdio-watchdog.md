# [OPEN] ESP-Hosted SDIO 从机连接与看门狗调试

## 症状
- `sdmmc_io_write_byte ... returned 0x107`
- `eh_reconfigure: bring-up timed out (2000ms)`
- `esp_hosted_connect_to_slave()` 返回 -1
- 随后 CPU1 的 `swdraw` 触发 Task WDT

## 假设
1. ESP32 从机未正常启动或复位时序不满足，导致 SDIO CMD52 写入失败。
2. SDIO 引脚、总线宽度或时钟配置与从机固件不匹配。
3. ESP-Hosted 失败后仍继续执行 LVGL/显示绘制任务，造成 CPU1 看门狗，而不是连接错误本身导致重启。
4. 主机与从机 ESP-Hosted 固件/协议版本不兼容。
5. 失败重试或初始化任务未正确退出，导致 SDIO/绘制任务资源竞争。

## 计划
- 先仅增加启动阶段和连接失败路径的运行日志，不修改业务逻辑。
- 用户重新烧录并提供完整日志后，确认假设。
- 根据证据实施最小修复，再做一次运行验证。

## 证据结论
- 主机后台任务在 3070ms 才开始执行复位，但 ESP-Hosted 在 3085ms 已完成 SDIO card init 并启动 RX task。
- `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=y` 与 `main.c` 中手动 `esp_hosted_init()`/`esp_hosted_connect_to_slave()` 并存。
- GPIO32 在手动释放后仍为 0，说明自动 SDIO 初始化与手动复位发生竞争。
- 假设 1、2 仍需修复后验证；假设 3 的 WDT 是链路失败后的连锁现象。
- 根因确认：ESP-Hosted 自动初始化和手动初始化重复执行，且手动复位破坏了已启动的 SDIO 数据路径。

## 后续证据
- SDIO 已成功连接：slave chip id 0x00 (esp32)，transport connect 返回 0。
- 主机报告 host=3.0.5、coprocessor=2.9.3，并明确提示 major version mismatch。
- 连接后的 auto_init 自动初始化了 wifi；随后 HCI TX 失败并出现 opcode 0xc03 超时。
- 经典蓝牙扫描返回 ESP_ERR_INVALID_STATE，是蓝牙 HCI/控制器尚未稳定或初始化失败的结果，不是扫描 API 首要问题。

## 当前修复
- 关闭 ESP-Hosted WiFi 功能和 WiFi auto init。
- 后台无线初始化改为只执行蓝牙，WiFi 不再与蓝牙并行或串行启动。
- 扫描前增加 `bsp_bt_is_initialized()` 检查。
- 从机仍必须升级到与主机匹配的 ESP-Hosted 3.0.5 固件。

## 状态
等待关闭 WiFi 和升级从机固件后的运行验证。
