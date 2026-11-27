# LiveAudioProcess

[中文](#中文) | [English](#english)

## 中文

Android 低延迟实时音频处理与路由工具，支持本机 DSP、USB Audio 和 Wi-Fi 音频传输。

## 功能

- 基于 Android AAudio 的低延迟音频输入与输出
- 内置麦克风音频来源选择：默认、MIC、CAMCORDER、VOICE_RECOGNITION、VOICE_COMMUNICATION、UNPROCESSED、VOICE_PERFORMANCE；自动保存，运行中切换会重开音频流
- 输入设备时钟校正：以本地输出设备为基准统计输入采样率，显示估算 Hz、漂移和补偿 ppm，通过平滑重采样补偿系统输入及 USB Host 输入；无本地输出时自动忽略补偿
- Wi-Fi 音频发送与接收
  - UDP / TCP 传输
  - PCM Float32、AAC-LC、Opus 编解码
  - 可调抖动缓冲和动态缓冲目标
  - 接收端基于 BUFFER 深度的音频时钟漂移校正
  - 校正幅度可调：1-200 ppm
- USB Audio / USB Host 音频设备支持
- 实时 DSP 处理链
  - 动态多段图形参数均衡器：自由添加 / 删除节点，可拖动频响曲线，支持峰值、高通、低通、陷波、带通、低搁架和高搁架
  - Convolution Reverb
  - Limiter
  - Loudness processing
- 测试音源、波形和电平监视
- 音频干声 / 湿声录音
- Jetpack Compose 用户界面

## 技术栈

- Kotlin
- C / C++
- Android AAudio
- JNI
- Jetpack Compose
- Opus
- FDK-AAC
- libusb / USB Audio

## 构建

```bash
./gradlew :app:assembleDebug
```

Windows:

```powershell
.\gradlew.bat :app:assembleDebug
```

### GitHub Actions 发布签名 APK

工作流文件为 `.github/workflows/build-release.yml`，在 GitHub 的 `Actions` 页面手动运行 `Build and Release APK` 即可编译并创建 Release。发布工作流要求使用固定的 Android 签名密钥，签名密钥不能提交到仓库。

首次配置签名密钥（PowerShell）：

```powershell
keytool -genkeypair -v -keystore LiveAudioProcess-release.jks -alias liveaudioprocess -keyalg RSA -keysize 2048 -validity 10000
$base64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes(".\LiveAudioProcess-release.jks"))
Set-Clipboard $base64
```

在仓库 `Settings → Secrets and variables → Actions` 中添加以下 Secrets：

- `ANDROID_KEYSTORE_BASE64`：上一步复制的 Base64 内容
- `ANDROID_KEYSTORE_PASSWORD`：生成密钥库时设置的密码
- `ANDROID_KEY_ALIAS`：`liveaudioprocess`
- `ANDROID_KEY_PASSWORD`：该别名对应的密码

请妥善备份 `.jks` 文件和密码；以后发布更新必须继续使用同一签名密钥，否则 Android 会把它视为不同应用。`.jks` 文件不应提交到 Git。工作流缺少任一签名 Secret 时会直接失败，不会发布未签名 APK。

## 使用说明

处理链中的图形参数均衡器不设固定频段数，按实际节点数量动态分配。初始为空，双击曲线空白处可在对应频率和增益位置添加节点，也可点击“添加节点”；支持删除选中节点、清空全部，节点及数量会自动保存。空 EQ 为平直直通，节点越多实时处理开销越大，实际可用数量取决于设备性能。点击编号或曲线节点选中频段，编号列表可横向滚动；左右拖动节点调频率，峰值和搁架类型还可上下拖动调增益。下方可选择滤波类型、调整 Q 值、单独旁路频段或复位本段。频率轴为对数刻度，增益范围为 ±24 dB，Q 范围为 0.1–20；高通和低通为二阶 12 dB/oct。上限频率随 DSP 采样率限制在奈奎斯特频率以下。

彩色细线表示各频段响应，白线表示已开启频段的合成响应；这是滤波器频响，不是实时 FFT 频谱。整体 DSP 或 EQ 关闭时曲线仅用于配置预览。原生处理使用双精度 IIR，参数改变时进行约 5 ms 的系数平滑，不引入额外的前视缓冲；曲线只在参数或采样率变化时重新计算。新 EQ 默认平直，不迁移旧 EQ 参数，其他 DSP 设置不受影响。

顶部固定显示引擎和录音控制。点击“开始引擎”启动；停止时需将左侧滑块向右拖到末端并松手，点击滑轨或未拖到位不会停止引擎。录音按钮在引擎启动后可用，录音时显示计时，再次点击停止录音。

选择“内置麦克风”后，可在“系统输入”面板设置音频来源，并在启动后查看 AAudio 返回的实际采集来源。默认选项保持原有采集行为；自定义来源需要 Android 9+，VOICE_PERFORMANCE 需要 Android 10+。设备可能对不同来源采用不同的麦克风和处理链，UNPROCESSED 的实际效果取决于硬件支持。

两台手机进行 Wi-Fi 音频传输时：

1. 发送端选择 Wi-Fi 输出并填写接收端 IP 和端口。
2. 接收端选择 Wi-Fi 输入并监听相同端口。
3. 两端使用相同的采样率、声道数和编码格式。
4. 如两台手机的音频时钟存在偏差，在接收端开启 `CLOCK DRIFT CORRECTION`。
5. 根据 BUFFER 稳定性调整校正幅度，范围为 `1-200 ppm`。

“输入设备时钟校正”默认开启，可在控制台单独关闭并自动保存。初次统计至少需要 5 秒；页面显示相对于输出设备时钟的输入采样率，不是以外部精密时钟测得的绝对频率。优先使用 AAudio 时间戳，不可用时使用回调帧计数；USB Host 使用传输帧计数，包含调度抖动，估算值会随统计窗口逐步稳定。补偿在输入端、DSP 之前进行，包含少量缓冲深度反馈，总补偿限幅 ±5000 ppm。没有本地音频输出、时钟失效或切换设备时，不使用过期估算；测试音源和 Wi-Fi 接收保留各自原有逻辑，Wi-Fi 接收的 BUFFER 时钟校正设置不变。

原生时钟校正模拟测试（需 GCC）：

```powershell
New-Item -ItemType Directory -Force app/build/clock-tests
gcc -std=c11 -O2 -Wall -Wextra -Werror -I app/src/main/cpp app/src/main/cpp/input_clock_correction.c app/src/test/cpp/input_clock_correction_test.c -lm -o app/build/clock-tests/input_clock_correction_test.exe
./app/build/clock-tests/input_clock_correction_test.exe
```

## GitHub Topics

```text
android
android-audio
audio-processing
low-latency-audio
audio-dsp
wifi-audio
usb-audio
aaudio
opus
aac
pcm
kotlin
cpp
jetpack-compose
```

## 状态

项目处于持续开发阶段。不同 Android 设备对 AAudio、USB Audio、电池电流计量和低延迟路径的支持可能存在差异。

## English

Android low-latency real-time audio processing and routing tool with built-in DSP, USB Audio, and Wi-Fi audio streaming.

### Features

- Low-latency audio input and output based on Android AAudio
- Built-in microphone source selection: default, MIC, CAMCORDER, VOICE_RECOGNITION, VOICE_COMMUNICATION, UNPROCESSED, and VOICE_PERFORMANCE; saved automatically, with stream reopening when changed during capture
- Input device clock correction: estimates and displays the input sample rate relative to the local output clock, then smoothly resamples system and USB Host input before DSP; bypassed when there is no local output clock
- Wi-Fi audio sender and receiver
  - UDP / TCP transport
  - PCM Float32, AAC-LC, and Opus codecs
  - Configurable jitter buffer and dynamic buffer target
  - Receiver-side audio clock drift correction based on buffer depth
  - Adjustable correction limit from 1 to 200 ppm
- USB Audio / USB Host device support
- Real-time DSP chain
  - Dynamic multiband graphical parametric equalizer with freely added/removed nodes, draggable responses, bell, high/low-pass, notch, band-pass, and high/low-shelf filters
  - Convolution reverb
  - Limiter
  - Loudness processing
- Test tone source, waveform, and level monitoring
- Dry / wet audio recording
- Jetpack Compose user interface

### Technology Stack

- Kotlin
- C / C++
- Android AAudio
- JNI
- Jetpack Compose
- Opus
- FDK-AAC
- libusb / USB Audio

### Build

```bash
./gradlew :app:assembleDebug
```

Windows:

```powershell
.\gradlew.bat :app:assembleDebug
```

### Usage

For Wi-Fi audio streaming between two phones:

1. On the sender, enable Wi-Fi output and enter the receiver IP address and port.
2. On the receiver, enable Wi-Fi input and listen on the same port.
3. Use the same sample rate, channel count, and codec on both devices.
4. Enable `CLOCK DRIFT CORRECTION` on the receiver when the two audio clocks drift apart.
5. Adjust the correction limit between `1-200 ppm` while watching BUFFER stability.

### GitHub Topics

```text
android
android-audio
audio-processing
low-latency-audio
audio-dsp
wifi-audio
usb-audio
aaudio
opus
aac
pcm
kotlin
cpp
jetpack-compose
```

### Status

This project is under active development. AAudio, USB Audio, battery current metering, and low-latency audio path support may vary across Android devices.
