# LiveAudioProcess

[中文](#中文) | [English](#english)

## 中文

Android 低延迟实时音频处理与路由工具，支持本机 DSP、USB Audio 和 Wi-Fi 音频传输。

## 功能

- 基于 Android AAudio 的低延迟音频输入与输出
- Wi-Fi 音频发送与接收
  - UDP / TCP 传输
  - PCM Float32、AAC-LC、Opus 编解码
  - 可调抖动缓冲和动态缓冲目标
  - 接收端基于 BUFFER 深度的音频时钟漂移校正
  - 校正幅度可调：1-200 ppm
- USB Audio / USB Host 音频设备支持
- 实时 DSP 处理链
  - 4 段参数均衡器
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

## 使用说明

两台手机进行 Wi-Fi 音频传输时：

1. 发送端选择 Wi-Fi 输出并填写接收端 IP 和端口。
2. 接收端选择 Wi-Fi 输入并监听相同端口。
3. 两端使用相同的采样率、声道数和编码格式。
4. 如两台手机的音频时钟存在偏差，在接收端开启 `CLOCK DRIFT CORRECTION`。
5. 根据 BUFFER 稳定性调整校正幅度，范围为 `1-200 ppm`。

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
- Wi-Fi audio sender and receiver
  - UDP / TCP transport
  - PCM Float32, AAC-LC, and Opus codecs
  - Configurable jitter buffer and dynamic buffer target
  - Receiver-side audio clock drift correction based on buffer depth
  - Adjustable correction limit from 1 to 200 ppm
- USB Audio / USB Host device support
- Real-time DSP chain
  - 4-band parametric equalizer
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
