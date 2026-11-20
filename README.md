# A Share of Anthropic — 数字金条固件

Waveshare ESP32-C6-Touch-LCD-1.9 上的纪念金条。底座取自官方 `08_FactoryProgram`（引脚和屏幕初始化参数），动画渲染是自己写的。

## 烧录（在你自己的电脑上）

```bash
pip install esptool
# 第一次烧录，或想“重铸”（清空已铸造的数据）时，先整片擦除：
esptool.py --chip esp32c6 erase_flash
esptool.py --chip esp32c6 -b 460800 write_flash 0x0 ant-ingot-merged.bin
```
只覆盖固件、不擦除的话，铸造记录（存在 NVS 里）会保留。

## 使用流程
1. 开机后屏幕上是一块毛坯锭，用手机扫卡片上的二维码，连上 Wi-Fi `A Share of Anthropic Setup`，配置页会自动弹出（没弹出就打开 192.168.4.1）。
2. 填写 Wi-Fi、成交价、成交时间（北京时间）、股数、可选铭文、DeepSeek Key，点 CHECK & PREPARE。
3. 屏幕上依次打勾：Wi-Fi、行情、DeepSeek。三项都通过后卡片变成 READY TO MINT。
4. 持续摇动加热，金条从暗红烧到白热，然后铸造完成。

## 铸造之后
- **摇一摇 / 短按 BOOT**：翻面，顺序是正面 → 每日一句 → 行情面。
- **行情面上点一下屏幕**：在三页之间切换
  - **今日**：现价、涨跌、市场状态（PRE/OPEN/AFTER/CLOSED）、分时走势（虚线是你的成本和昨收）、日内区间、成交量，盘前盘后价，以及距开盘/收盘的倒计时
  - **持有以来**：从买入那一刻起的走势（起点有一个小金锭标记）、持有天数、期间最高和最低、盈亏
  - **自 IPO**：从 $83 发行价起的走势、市值、你的入场价、持股占比
  - 单日涨跌超过 ±5% 时，右上角会出现一个小菱形，不会有其他提醒
- **RGB 灯**：美股正常交易时段极暗地呼吸，现价高于成本是暖金色，低于是冷灰蓝，休市熄灭。
- **长按屏幕**：尺度彩蛋（从你这一股一直缩放到全部股本），点一下返回。
- **长按 BOOT 2 秒**：进入设置模式。手机连 `A Share of Anthropic Setup`，或者在同一局域网里打开屏幕底部显示的 IP。设置页可以改 Wi-Fi 和 DeepSeek Key，调手感参数（实时生效，点 SAVE FEEL 保存），彩排铸造，预览包浆年份。

## 目录
- `main/core/`：可移植的渲染核心，金属、扭索纹、动画全在这里（PC 和板子共用）
- `main/*.c`：板级代码，包括屏幕、IMU、触摸、灯、Wi-Fi/门户、行情、DeepSeek、蓝牙
- `host/`：PC 模拟器，`make full.mp4` 渲染完整流程视频，`make market.mp4` 用 `mkt.txt` 里的真实行情渲染行情面
- `preview/`：预览视频和关键帧
- `tools/gen_fonts.py`：字体光栅化（Cinzel、EB Garamond、IBM Plex Mono，均为 OFL 授权）

## 数据来源
- 行情：腾讯 `qt.gtimg.cn`（报价），`web.ifzq.gtimg.cn`（分时和日线），新浪 `hq.sinajs.cn`（盘前盘后价），全部免 key
- 刷新频率：开盘时段报价每 15 秒、分时每 1 分钟；盘前盘后每 1 分钟；休市每 10 分钟；日线每 6 小时一次，收盘后会额外刷新一次
- 每日一句：DeepSeek `deepseek-chat`，每天一次（在北京时间换日后获取）
