package com.llawsxx.audioprocess

import android.content.BroadcastReceiver
import android.content.Context
import android.content.ContentValues
import android.content.ContentResolver
import android.content.Intent
import android.content.IntentFilter
import android.app.PendingIntent
import android.media.AudioDeviceInfo
import android.media.AudioDeviceCallback
import android.media.AudioManager
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.provider.MediaStore
import android.os.Environment
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import java.io.File
import android.os.ParcelFileDescriptor
import kotlin.math.roundToInt

class AudioEngine(private val context: Context) {
    private val wifiConfigurationFailureNotice = "Wi-Fi 音频配置失败，所选 Wi-Fi 路由未生效"
    private val usbVolumeFailureNotice = "USB 音频设备不支持主音量控制，或音量命令发送失败"
    private val systemVolumeFailureNotice = "系统媒体音量调节失败"
    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val routeHandler = Handler(Looper.getMainLooper())
    private val usbAutoPermissionAction = "${context.packageName}.USB_AUDIO_PERMISSION_AUTO"
    @Volatile private var activeInputDeviceId = -1
    @Volatile private var activeOutputDeviceId = -1
    @Volatile private var bluetoothRouted = false
    private var observedBluetoothDeviceId = Int.MIN_VALUE
    private var bluetoothRetryCount = 0
    private var nextBluetoothRetryAtMs = 0L
    @Volatile private var networkRole = 0
    @Volatile private var wifiFallbackActive = false
    @Volatile private var wifiReconnectCount = 0
    @Volatile private var wifiReconnectPending = false
    @Volatile private var displayedWifiErrorNotice: String? = null
    @Volatile var wifiInputTimeoutMs = 1_000
    @Volatile var wifiClockCorrectionEnabled = false
    @Volatile var wifiDynamicBufferEnabled = true
    @Volatile var wifiManualBufferBias = 0.5f
    @Volatile var wifiOpusFrameMs = 20
    @Volatile var wifiOpusProfile = 0
    @Volatile var routeNotice: String? = null
        private set
    private val deviceCallback = if (Build.VERSION.SDK_INT >= 23) object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) { scheduleRouteRefresh() }
        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) { scheduleRouteRefresh() }
    } else null
    private val usbDeviceReceiver = object : BroadcastReceiver() {
        override fun onReceive(receiverContext: Context?, intent: Intent?) {
            when (intent?.action) {
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    if (!requestUsbHostPermissionIfNeeded()) scheduleRouteRefresh()
                }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> scheduleRouteRefresh()
                usbAutoPermissionAction -> {
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false) && isRunning) {
                        routeHandler.removeCallbacks(routeRestart)
                        routeHandler.post(routeRestart)
                    } else scheduleRouteRefresh()
                }
            }
        }
    }

    init {
        if (Build.VERSION.SDK_INT >= 23)
            deviceCallback?.let { audioManager.registerAudioDeviceCallback(it, routeHandler) }
        val usbFilter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(usbAutoPermissionAction)
        }
        if (Build.VERSION.SDK_INT >= 33)
            context.registerReceiver(usbDeviceReceiver, usbFilter, Context.RECEIVER_EXPORTED)
        else {
            @Suppress("DEPRECATION")
            context.registerReceiver(usbDeviceReceiver, usbFilter)
        }
    }
    @Volatile var inputSource = InputSource.BUILT_IN
        internal set
    @Volatile var toneWaveform = 0
    @Volatile var toneMusic = 0
    @Volatile var toneChannels = 0
    @Volatile var toneFrequency = 1000f
    @Volatile var toneFrequency2 = 20000f
    @Volatile var toneDurationSeconds = 10f
    @Volatile var toneClickIntervalMs = 1000f
    @Volatile var toneLevel = .25f
    @Volatile var outputSource = OutputSource.SPEAKER
        internal set
    @Volatile var outputVolumePercent = 100
    @Volatile var usbOutputHostActive = false
        private set
    @Volatile var sampleRate = 48_000; @Volatile var bufferFrames = 256; @Volatile var inputPair = 0
    @Volatile var outputSampleRate = 48_000
        private set
    @Volatile var usbInputRate = 48_000
        private set
    @Volatile var usbOutputRate = 48_000
        private set
    @Volatile var usbInputBitDepth = 16
        private set
    @Volatile var usbOutputBitDepth = 16
        private set
    @Volatile var usbInputBurstPackets = 8
        private set
    @Volatile var usbOutputBurstPackets = 8
        private set
    @Volatile var usbInputBufferMaxMs = 20
        private set
    @Volatile var systemOutputBufferMaxMs = 40
        private set
    @Volatile var systemInputBufferMaxMs = 20
        private set
    @Volatile var eqGain = 2f; @Volatile var eqFrequency = 1200f; @Volatile var eqQ = .85f
    @Volatile var eq2Frequency = 250f; @Volatile var eq2Gain = 0f; @Volatile var eq2Q = 1f
    @Volatile var eq3Frequency = 4000f; @Volatile var eq3Gain = 0f; @Volatile var eq3Q = 1f
    @Volatile var eq4Frequency = 10000f; @Volatile var eq4Gain = 0f; @Volatile var eq4Q = 1f
    @Volatile var reverbRoom = 42f; @Volatile var reverbDecay = 1.8f; @Volatile var reverbDamping = 35f; @Volatile var reverbMix = .18f
    @Volatile var limiterInputGain = 0f; @Volatile var limiterThreshold = -.5f; @Volatile var limiterRelease = 80f; @Volatile var limiterCeiling = -.5f; @Volatile var limiterLookAhead = 1f; @Volatile var limiterAdaptiveRelease = false
    @Volatile var loudnessTarget = -16f; @Volatile var loudnessLra = 7f; @Volatile var loudnessTruePeak = -1f
    @Volatile var dspEnabled = false; @Volatile var eqEnabled = true; @Volatile var reverbEnabled = true; @Volatile var limiterEnabled = true; @Volatile var loudnessEnabled = true
    @Volatile var isRunning = false; private set
    @Volatile var isRecording = false; private set
    @Volatile var lastError: String? = null; private set
    private data class RecordingTarget(val dryUri: android.net.Uri?, val wetUri: android.net.Uri?, val dryPfd: ParcelFileDescriptor, val wetPfd: ParcelFileDescriptor)
    private var recordingTarget: RecordingTarget? = null
    private var usbConnection: UsbDeviceConnection? = null
    private fun socketErrorText(error: Int): String = when (error) {
        13 -> "权限不足"
        98 -> "地址或端口已被占用"
        99 -> "本机不存在该 IP 地址"
        101 -> "网络不可达"
        110 -> "连接超时"
        111 -> "连接被拒绝"
        113 -> "找不到到目标主机的路由"
        else -> "系统错误 $error"
    }
    private fun codecName(value: Int): String = when (value) { 0 -> "PCM"; 1 -> "AAC"; 2 -> "Opus"; else -> "未知($value)" }
    private fun nativeWifiError(): Pair<Int, String?> {
        val info = runCatching { NativeAudio.networkErrorInfo() }.getOrDefault(IntArray(0))
        val code = info.getOrElse(0) { 0 }
        val actual = info.getOrElse(1) { 0 }
        val expected = info.getOrElse(2) { 0 }
        val message = when (code) {
            0 -> null
            1 -> "Wi-Fi 配置失败：收发角色参数无效（$actual）"
            2 -> "Wi-Fi 配置失败：传输协议参数无效（$actual）"
            3 -> "Wi-Fi 配置失败：编码参数无效（$actual）"
            4 -> "Wi-Fi 配置失败：不支持 $actual Hz 采样率"
            5 -> "Wi-Fi 配置失败：IP 地址格式无效"
            6 -> "Wi-Fi 配置失败：端口必须在 1–65535 之间（当前 $actual）"
            7 -> "Wi-Fi 配置失败：AAC 初始化失败（${actual} Hz，${expected} bps）"
            8 -> "Wi-Fi 配置失败：无法创建网络套接字（${socketErrorText(actual)}）"
            9 -> "Wi-Fi 配置失败：端口 $actual 已被占用"
            10 -> "Wi-Fi 配置失败：无法绑定端口 $actual（${socketErrorText(expected)}）"
            11 -> "Wi-Fi 配置失败：无法监听端口 $actual（${socketErrorText(expected)}）"
            12 -> "Wi-Fi 输出连接端口 $expected 失败（${socketErrorText(actual)}）"
            13 -> "Wi-Fi 输入已建立 TCP 连接，但尚未收到完整音频数据"
            20 -> "Wi-Fi 数据包过短（收到 $actual 字节，至少需要 $expected 字节）"
            21 -> "Wi-Fi 协议标识不匹配：对端不是本应用的音频协议"
            22 -> "Wi-Fi 协议版本不一致（发送端 v$actual，接收端 v$expected）"
            23 -> "Wi-Fi 编码不一致（发送端 ${codecName(actual)}，接收端 ${codecName(expected)}）"
            24 -> "Wi-Fi 声道数不一致（发送端 $actual，接收端 $expected）"
            25 -> "Wi-Fi 采样率不一致（发送端 $actual Hz，接收端 $expected Hz）"
            26 -> "Wi-Fi 音频帧长不一致（发送端 $actual，接收端 $expected）"
            27 -> if (actual > 0) "Wi-Fi PCM 位深/格式不一致（发送端每样本 $actual 字节，接收端要求 Float32/$expected 字节）" else "Wi-Fi PCM 数据布局不一致（接收端要求 Float32）"
            28 -> "Wi-Fi AAC 数据长度异常（收到 $actual 字节，上限 $expected 字节）"
            29 -> "Wi-Fi AAC 解码失败（返回 $actual，期望 $expected 帧）"
            30 -> "Wi-Fi TCP 包长度字段异常（$actual 字节，上限 $expected 字节）"
            31 -> "Wi-Fi 接收失败（${socketErrorText(actual)}）"
            32 -> "Wi-Fi 发送端已断开连接"
            33 -> "Wi-Fi 配置失败：Opus 初始化失败（${actual} Hz，${expected} bps）"
            34 -> "Wi-Fi Opus 数据长度异常（收到 $actual 字节，上限 $expected 字节）"
            35 -> "Wi-Fi Opus 解码失败（返回 $actual，期望 $expected 帧）"
            else -> "Wi-Fi 音频错误（代码 $code，详情 $actual/$expected）"
        }
        return code to message
    }
    private fun showWifiError(message: String) {
        displayedWifiErrorNotice = message
        routeNotice = message
    }
    private fun clearDisplayedWifiError() {
        val displayed = displayedWifiErrorNotice
        if (displayed != null && routeNotice == displayed) routeNotice = null
        displayedWifiErrorNotice = null
    }
    fun configureNetwork(role: Int, transport: Int, codec: Int, bitrate: Int, host: String, port: Int, packetDurationMs: Int, minBufferMs: Int, maxBufferMs: Int, maxHoldMs: Int): Boolean {
        val previousNetworkRole = networkRole
        NativeAudio.configureNetworkOpus(wifiOpusFrameMs, when (wifiOpusProfile) {
            1 -> 2048
            2 -> 2051
            else -> 2049
        })
        val configured = NativeAudio.configureNetwork(role, transport, codec, sampleRate, bitrate, host, port, packetDurationMs, minBufferMs, maxBufferMs, maxHoldMs)
        val nativeError = nativeWifiError()
        if (!configured) NativeAudio.clearNetwork()
        NativeAudio.configureNetworkClockCorrection(configured && wifiClockCorrectionEnabled && role == 2)
        NativeAudio.configureNetworkTargetBuffer(wifiDynamicBufferEnabled, (wifiManualBufferBias.coerceIn(0f, 1f) * 1000f).roundToInt())
        networkRole = if (configured) role else 0
        if (role != 1) { wifiReconnectCount = 0; wifiReconnectPending = false }
        if (!configured) showWifiError(nativeError.second ?: wifiConfigurationFailureNotice)
        else if (nativeError.first == 0) clearDisplayedWifiError()
        if (isRunning && inputSource == InputSource.WIFI && previousNetworkRole != networkRole) {
            routeNotice = if (networkRole == 2) "Wi-Fi 输入已恢复，正在重新连接音频流" else "Wi-Fi 输入不可用，正在切换音频流"
            routeHandler.removeCallbacks(routeRestart)
            routeHandler.post(routeRestart)
        }
        return configured
    }
    fun configureWifiBufferTarget(dynamic: Boolean, bias: Float) {
        wifiDynamicBufferEnabled = dynamic
        wifiManualBufferBias = bias.coerceIn(0f, 1f)
        if (NativeAudio.available) {
            NativeAudio.configureNetworkTargetBuffer(dynamic, (wifiManualBufferBias * 1000f).roundToInt())
        }
    }
    fun configureWifiOpus(frameMs: Int, profile: Int) {
        wifiOpusFrameMs = frameMs.coerceIn(5, 60)
        wifiOpusProfile = profile.coerceIn(0, 2)
        if (NativeAudio.available) NativeAudio.configureNetworkOpus(wifiOpusFrameMs, when (wifiOpusProfile) {
            1 -> 2048
            2 -> 2051
            else -> 2049
        })
    }
    fun clearNetwork() {
        NativeAudio.clearNetwork()
        NativeAudio.configureNetworkClockCorrection(false)
        networkRole = 0
        wifiFallbackActive = false
        wifiReconnectCount = 0
        wifiReconnectPending = false
        displayedWifiErrorNotice = null
        if (routeNotice?.startsWith("Wi-Fi ") == true) routeNotice = null
    }
    fun configureUsbOutputBuffer(maxBufferMs: Int) {
        if (!NativeAudio.available) return
        NativeAudio.configureUsbOutputBuffer(maxBufferMs.coerceIn(5, 200))
    }
    fun configureUsbInputBuffer(maxBufferMs: Int) {
        val normalized = maxBufferMs.coerceIn(5, 200)
        usbInputBufferMaxMs = normalized
        if (NativeAudio.available) NativeAudio.configureUsbInputBuffer(normalized)
    }
    fun configureAudioFormat(requestedSampleRate: Int, requestedOutputSampleRate: Int, requestedUsbInputBitDepth: Int, requestedUsbOutputBitDepth: Int) {
        val normalizedRate = requestedSampleRate.takeIf { it == 44_100 || it == 48_000 || it == 96_000 } ?: 48_000
        val normalizeRate = { value: Int -> value.takeIf { it == 44_100 || it == 48_000 || it == 96_000 } ?: 48_000 }
        val normalizeBits = { value: Int -> value.takeIf { it == 16 || it == 24 || it == 32 } ?: 16 }
        val normalizedOutputRate = normalizeRate(requestedOutputSampleRate)
        val normalizedInputBits = normalizeBits(requestedUsbInputBitDepth)
        val normalizedOutputBits = normalizeBits(requestedUsbOutputBitDepth)
        val rateChanged = sampleRate != normalizedRate
        val outputRateChanged = outputSampleRate != normalizedOutputRate
        val usbBitDepthChanged = usbInputBitDepth != normalizedInputBits || usbOutputBitDepth != normalizedOutputBits
        sampleRate = normalizedRate
        outputSampleRate = normalizedOutputRate
        usbInputRate = normalizedRate
        usbOutputRate = normalizedOutputRate
        usbInputBitDepth = normalizedInputBits
        usbOutputBitDepth = normalizedOutputBits
        if (isRunning && (rateChanged || outputRateChanged || (usbBitDepthChanged && (inputSource == InputSource.USB || outputSource == OutputSource.USB)))) {
            routeNotice = "正在应用新的音频格式"
            routeHandler.removeCallbacks(routeRestart)
            routeHandler.post(routeRestart)
        }
    }
    fun configureAudioFormat(requestedSampleRate: Int, requestedUsbBitDepth: Int) = configureAudioFormat(requestedSampleRate, requestedSampleRate, requestedUsbBitDepth, requestedUsbBitDepth)
    fun configureUsbBursts(inputPackets: Int, outputPackets: Int) {
        fun normalize(value: Int) = value.takeIf { it == 1 || it == 2 || it == 4 || it == 8 || it == 16 || it == 24  || it == 32  || it == 48  || it == 64  || it == 128 } ?: 8
        val normalizedInput = normalize(inputPackets)
        val normalizedOutput = normalize(outputPackets)
        if (usbInputBurstPackets == normalizedInput && usbOutputBurstPackets == normalizedOutput) return
        usbInputBurstPackets = normalizedInput
        usbOutputBurstPackets = normalizedOutput
        if (isRunning && (inputSource == InputSource.USB || outputSource == OutputSource.USB)) {
            restartStreamsForRouteChange()
        }
    }
    fun configureSystemOutputBuffer(maxMs: Int) {
        val normalized = maxMs.coerceIn(5, 200)
        systemOutputBufferMaxMs = normalized
        if (NativeAudio.available) NativeAudio.configureOutputBufferMaxMs(normalized)
    }
    fun configureSystemInputBuffer(maxMs: Int) {
        val normalized = maxMs.coerceIn(5, 200)
        systemInputBufferMaxMs = normalized
        if (NativeAudio.available) NativeAudio.configureInputBufferMaxMs(normalized)
    }
    fun setOutputVolumePercent(percent: Int): Boolean {
        val normalized = percent.coerceIn(0, 100)
        val useUsbVolume = outputSource == OutputSource.USB && isRunning &&
            NativeAudio.routeInfo().getOrElse(1) { -1 } == -4
        val applied = if (useUsbVolume) {
            NativeAudio.available && NativeAudio.setUsbVolume(normalized)
        } else {
            runCatching {
                val max = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
                val level = (max * normalized / 100f).roundToInt().coerceIn(0, max)
                audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, level, 0)
                true
            }.getOrDefault(false)
        }
        if (!applied) routeNotice = if (useUsbVolume) usbVolumeFailureNotice else systemVolumeFailureNotice
        else if (routeNotice == usbVolumeFailureNotice || routeNotice == systemVolumeFailureNotice) routeNotice = null
        return applied
    }
    fun systemVolumePercent(): Int = runCatching {
        val max = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
        if (max <= 0) 0 else (audioManager.getStreamVolume(AudioManager.STREAM_MUSIC) * 100f / max).roundToInt().coerceIn(0, 100)
    }.getOrDefault(0)
    private fun usbInputDevice(): AudioDeviceInfo? {
        return audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS).firstOrNull { it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET }
    }
    private fun usbOutputDevice(): AudioDeviceInfo? {
        return audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET }
    }
    private fun usbAudioDevice() = (context.getSystemService(Context.USB_SERVICE) as UsbManager)
        .deviceList.values.firstOrNull { usbDevice ->
            (0 until usbDevice.interfaceCount).any {
                usbDevice.getInterface(it).interfaceClass == android.hardware.usb.UsbConstants.USB_CLASS_AUDIO
            }
        }
    private fun requestUsbHostPermissionIfNeeded(): Boolean {
        if (!isRunning || (inputSource != InputSource.USB && outputSource != OutputSource.USB)) return false
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val device = usbAudioDevice() ?: return false
        if (manager.hasPermission(device)) return false
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
            if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        val permissionIntent = PendingIntent.getBroadcast(
            context, 1001, Intent(usbAutoPermissionAction).setPackage(context.packageName), flags
        )
        manager.requestPermission(device, permissionIntent)
        return true
    }
    private fun openUsbHostConnection(): Int {
        usbConnection?.close()
        usbConnection = null
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val device = usbAudioDevice() ?: return -1
        if (!manager.hasPermission(device)) return -1
        val connection = manager.openDevice(device) ?: return -1
        usbConnection = connection
        return connection.fileDescriptor
    }
    private fun bluetoothOutputDevice(): AudioDeviceInfo? {
        return runCatching {
            audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
                .filter { isBluetoothOutput(it) }
                .minByOrNull { bluetoothOutputPriority(it) }
        }.getOrNull()
    }
    private fun isBluetoothOutput(device: AudioDeviceInfo): Boolean {
        return device.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
            device.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
            (Build.VERSION.SDK_INT >= 28 && device.type == AudioDeviceInfo.TYPE_HEARING_AID) ||
            (Build.VERSION.SDK_INT >= 31 && (device.type == AudioDeviceInfo.TYPE_BLE_HEADSET || device.type == AudioDeviceInfo.TYPE_BLE_SPEAKER))
    }
    private fun bluetoothOutputPriority(device: AudioDeviceInfo): Int = when (device.type) {
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> 0
        AudioDeviceInfo.TYPE_BLE_HEADSET, AudioDeviceInfo.TYPE_BLE_SPEAKER -> 1
        AudioDeviceInfo.TYPE_HEARING_AID -> 2
        else -> 3
    }
    private fun prepareBluetoothRoute(): Int {
        val device = bluetoothOutputDevice() ?: return -1
        if (Build.VERSION.SDK_INT >= 31) {
            runCatching {
                audioManager.availableCommunicationDevices.firstOrNull { it.id == device.id }
                    ?.let { audioManager.setCommunicationDevice(it) }
            }
        } else if (device.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) {
            runCatching {
                audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
                audioManager.startBluetoothSco()
                audioManager.isBluetoothScoOn = true
            }
        }
        bluetoothRouted = true
        return device.id
    }
    private fun clearBluetoothRoute() {
        if (!bluetoothRouted) return
        if (Build.VERSION.SDK_INT >= 31) runCatching { audioManager.clearCommunicationDevice() }
        else {
            runCatching {
                audioManager.isBluetoothScoOn = false
                audioManager.stopBluetoothSco()
                audioManager.mode = AudioManager.MODE_NORMAL
            }
        }
        bluetoothRouted = false
    }
    private val routeRefresh = Runnable {
        if (!isRunning) return@Runnable
        val inputId = if (inputSource == InputSource.USB) usbInputDevice()?.id ?: -1 else -1
        val outputId = when (outputSource) {
            OutputSource.USB -> usbOutputDevice()?.id ?: -1
            OutputSource.BLUETOOTH -> bluetoothOutputDevice()?.id ?: -1
            else -> -1
        }
        if (inputId != activeInputDeviceId || outputId != activeOutputDeviceId) restartStreamsForRouteChange()
    }
    private val routeRestart = Runnable { if (isRunning) restartStreamsForRouteChange() }
    private val bluetoothRouteMonitor = object : Runnable {
        override fun run() {
            if (!isRunning || outputSource != OutputSource.BLUETOOTH) return
            val desiredDeviceId = bluetoothOutputDevice()?.id ?: -1
            if (desiredDeviceId != observedBluetoothDeviceId) {
                observedBluetoothDeviceId = desiredDeviceId
                bluetoothRetryCount = 0
                nextBluetoothRetryAtMs = 0L
            }

            val actualOutputDeviceId = NativeAudio.routeInfo().getOrElse(1) { -1 }
            if (desiredDeviceId != activeOutputDeviceId) {
                routeNotice = if (desiredDeviceId < 0) {
                    "蓝牙输出已断开，当前使用系统默认输出；设备恢复后将自动重连"
                } else {
                    "检测到蓝牙设备，正在自动重连"
                }
                restartStreamsForRouteChange()
                return
            }

            if (desiredDeviceId >= 0 && actualOutputDeviceId != desiredDeviceId) {
                val now = SystemClock.elapsedRealtime()
                if (bluetoothRetryCount < BLUETOOTH_ROUTE_RETRY_LIMIT && now >= nextBluetoothRetryAtMs) {
                    val retryDelayMs = (BLUETOOTH_ROUTE_RETRY_BASE_MS shl bluetoothRetryCount).coerceAtMost(BLUETOOTH_ROUTE_RETRY_MAX_MS)
                    bluetoothRetryCount++
                    nextBluetoothRetryAtMs = now + retryDelayMs
                    routeNotice = "蓝牙路由尚未生效，正在自动重连（$bluetoothRetryCount/$BLUETOOTH_ROUTE_RETRY_LIMIT）"
                    restartStreamsForRouteChange()
                    return
                }
            } else if (desiredDeviceId >= 0) {
                bluetoothRetryCount = 0
                nextBluetoothRetryAtMs = 0L
            }

            refreshRouteNotice()
            routeHandler.postDelayed(this, BLUETOOTH_MONITOR_INTERVAL_MS)
        }
    }
    private val wifiHealthMonitor = object : Runnable {
        override fun run() {
            if (!isRunning) return
            if (NativeAudio.usbHostFailed()) {
                routeNotice = "USB 声卡已断开，正在切换到系统默认输入/输出"
                restartStreamsForRouteChange()
                return
            }
            val inputStreamError = NativeAudio.inputInfo().getOrElse(12) { 0L }.toInt()
            val outputStreamError = NativeAudio.outputInfo().getOrElse(12) { 0L }.toInt()
            if (inputStreamError < 0 || outputStreamError < 0) {
                val failedSide = when {
                    inputStreamError < 0 && outputStreamError < 0 -> "输入和输出"
                    inputStreamError < 0 -> "输入"
                    else -> "输出"
                }
                val error = if (outputStreamError < 0) outputStreamError else inputStreamError
                routeNotice = "AAudio $failedSide 流已断开（错误码 $error），正在重新连接音频设备"
                restartStreamsForRouteChange()
                return
            }
            val nativeError = nativeWifiError()
            if (networkRole == 1) {
                if (nativeError.first == 8 || nativeError.first == 12) {
                    showWifiError("${nativeError.second}，正在自动重连")
                }
                val timeout = NativeAudio.networkOutputTimedOut(wifiInputTimeoutMs.coerceIn(100, 60_000))
                if (timeout && !wifiReconnectPending) {
                    wifiReconnectCount = (NativeAudio.networkOutputConnectAttempts() - 1).coerceAtLeast(1)
                    wifiReconnectPending = true
                    showWifiError("${nativeError.second ?: "Wi-Fi 输出超过 ${wifiInputTimeoutMs} ms 无法发送"}，第 ${wifiReconnectCount} 次重连中")
                }
                if (wifiReconnectPending) {
                    val attempts = (NativeAudio.networkOutputConnectAttempts() - 1).coerceAtLeast(1)
                    if (attempts > wifiReconnectCount) {
                        wifiReconnectCount = attempts
                        showWifiError("${nativeWifiError().second ?: "Wi-Fi 输出超过 ${wifiInputTimeoutMs} ms 无法发送"}，第 ${wifiReconnectCount} 次重连中")
                    }
                }
                if (wifiReconnectPending && NativeAudio.networkOutputConnected()) {
                    wifiReconnectPending = false
                    clearDisplayedWifiError()
                }
                if (!wifiReconnectPending && nativeError.first == 0 && NativeAudio.networkOutputConnected()) clearDisplayedWifiError()
            }
            if (inputSource == InputSource.WIFI && networkRole == 2) {
                val nativeErrorMessage = nativeError.second
                if (nativeError.first in 20..32 && nativeErrorMessage != null) showWifiError(nativeErrorMessage)
                val timedOut = NativeAudio.networkInputTimedOut(wifiInputTimeoutMs.coerceIn(100, 60_000))
                if (timedOut && !wifiFallbackActive) {
                    wifiFallbackActive = true
                    restartStreamsForRouteChange()
                    showWifiError("${nativeError.second ?: "Wi-Fi 输入超过 ${wifiInputTimeoutMs} ms 无数据"}，已自动切换到默认麦克风")
                    return
                }
                if (!timedOut && wifiFallbackActive) {
                    wifiFallbackActive = false
                    restartStreamsForRouteChange()
                    clearDisplayedWifiError()
                    routeNotice = "Wi-Fi 输入已恢复，已自动切回 Wi-Fi 音频"
                    return
                }
                if (!timedOut && nativeError.first == 0) clearDisplayedWifiError()
            }
            routeHandler.postDelayed(this, 100)
        }
    }
    private fun scheduleRouteRefresh() {
        routeHandler.removeCallbacks(routeRefresh)
        routeHandler.postDelayed(routeRefresh, 250)
    }
    fun updateRouting(input: InputSource, output: OutputSource, pair: Int) {
        val changed = inputSource != input || outputSource != output || inputPair != pair
        inputSource = input
        outputSource = output
        inputPair = pair
        if (input != InputSource.WIFI) wifiFallbackActive = false
        if (changed && isRunning) {
            routeNotice = "正在应用新的音频路由"
            routeHandler.removeCallbacks(routeRestart)
            routeHandler.post(routeRestart)
        }
    }
    /** Restart only AAudio/DSP streams; keep the native recording files open. */
    private fun restartStreamsForRouteChange() {
        if (!isRunning) return
        routeHandler.removeCallbacks(bluetoothRouteMonitor)
        NativeAudio.stopForRouteChange()
        clearBluetoothRoute()
        activeInputDeviceId = -1
        activeOutputDeviceId = -1
        isRunning = false
        start()
    }
    fun availableInputPairs(): List<ChannelPair> {
        if (!NativeAudio.available) return listOf(ChannelPair(0, "AAudio unavailable"))
        val usb = usbInputDevice()
        // Some USB audio devices report only a mono capability through
        // AudioManager even though their capture endpoint is stereo.
        val count = maxOf(usb?.channelCounts?.maxOrNull()?.coerceIn(1, 8) ?: 2, 2)
        return if (count < 2) listOf(ChannelPair(0, "Mono")) else (0 until count / 2).map { ChannelPair(it, "CH ${it * 2 + 1}/${it * 2 + 2}") }
    }
    fun start() {
        if (isRunning) return
        lastError = null
        usbOutputHostActive = false
        if (!NativeAudio.available) { lastError = "AAudio requires Android 8.0 or newer"; return }
        if (inputSource == InputSource.TEST_TONE && outputSource == OutputSource.NONE) {
            outputSource = OutputSource.SPEAKER
            lastError = "测试 Tone 需要本地输出，已切换到扬声器"
        }
        val usbInput = if (inputSource == InputSource.USB) usbInputDevice() else null
        val pairs = availableInputPairs()
        val channels = if (inputSource == InputSource.USB && usbInput != null) (pairs.size * 2).coerceIn(2, 8) else 1
        pushNativeParameters()
        val nativeInputChannels = if (inputSource == InputSource.WIFI) 2 else channels
        val inputDeviceId = usbInput?.id ?: -1
        val outputDeviceId = when (outputSource) {
            OutputSource.USB -> usbOutputDevice()?.id ?: -1
            OutputSource.BLUETOOTH -> prepareBluetoothRoute()
            else -> -1
        }
        val usbPresent = usbAudioDevice() != null
        if (inputSource == InputSource.USB && inputDeviceId < 0) {
            lastError = if (usbPresent) "当前 USB 声卡不支持输入（可能是仅输出声卡），暂使用默认麦克风"
            else "USB 输入已断开，暂使用默认麦克风"
        }
        if (outputSource == OutputSource.USB && outputDeviceId < 0) {
            lastError = if (usbPresent) "当前 USB 声卡不支持输出（可能是仅输入声卡），暂使用默认扬声器"
            else "USB 输出已断开，暂使用默认扬声器"
        }
        if (outputSource == OutputSource.BLUETOOTH && outputDeviceId < 0) lastError = "未检测到蓝牙耳机，暂使用默认输出"
        val useNetworkInput = inputSource == InputSource.WIFI && networkRole == 2 && !wifiFallbackActive
        val requestedUsbInputHost = inputSource == InputSource.USB
        val requestedUsbOutputHost = outputSource == OutputSource.USB
        val usbFd = if (requestedUsbInputHost || requestedUsbOutputHost) openUsbHostConnection() else -1
        val usbInputHost = requestedUsbInputHost && usbFd >= 0
        val usbOutputHost = requestedUsbOutputHost && usbFd >= 0
        val enableOutput = outputSource != OutputSource.NONE
        var started = NativeAudio.start(sampleRate, outputSampleRate, bufferFrames, inputDeviceId, outputDeviceId, enableOutput, nativeInputChannels, inputPair, useNetworkInput, usbFd, usbInputHost, usbOutputHost, usbInputBitDepth, usbOutputBitDepth, usbInputBurstPackets, usbOutputBurstPackets)
        if (!started && usbFd >= 0) {
            usbConnection?.close()
            usbConnection = null
            lastError = "USB Host 格式不可用，正在回退到系统 USB 音频"
            started = NativeAudio.start(sampleRate, outputSampleRate, bufferFrames, inputDeviceId, outputDeviceId, enableOutput, nativeInputChannels, inputPair, useNetworkInput, -1, false, false, usbInputBitDepth, usbOutputBitDepth, usbInputBurstPackets, usbOutputBurstPackets)
        }
        if (!started && inputSource == InputSource.USB && nativeInputChannels == 2) {
            // Keep USB usable on devices whose driver rejects a stereo AAudio request.
            started = NativeAudio.start(sampleRate, outputSampleRate, bufferFrames, inputDeviceId, outputDeviceId, enableOutput, 1, inputPair, useNetworkInput, -1, false, false, usbInputBitDepth, usbOutputBitDepth, usbInputBurstPackets, usbOutputBurstPackets)
            if (started) lastError = "USB 输入驱动拒绝立体声，已回退为单声道"
        }
        if (started) isRunning = true
        else { clearBluetoothRoute(); lastError = "AAudio stream open failed; check microphone and speaker settings" }
        if (isRunning) {
            activeInputDeviceId = inputDeviceId
            activeOutputDeviceId = outputDeviceId
            usbOutputHostActive = NativeAudio.routeInfo().getOrElse(1) { -1 } == -4
            refreshRouteNotice()
            if (usbOutputHostActive) setOutputVolumePercent(outputVolumePercent)
            routeHandler.removeCallbacks(wifiHealthMonitor)
            routeHandler.postDelayed(wifiHealthMonitor, 100)
            routeHandler.removeCallbacks(bluetoothRouteMonitor)
            if (outputSource == OutputSource.BLUETOOTH) routeHandler.postDelayed(bluetoothRouteMonitor, BLUETOOTH_MONITOR_INTERVAL_MS)
        }
    }
    private fun refreshRouteNotice() {
        if (!isRunning) return
        val actual = NativeAudio.routeInfo()
        val actualInput = actual.getOrElse(0) { -1 }
        val actualOutput = actual.getOrElse(1) { -1 }
        val actualInputChannels = actual.getOrElse(2) { -1 }
        val usbInputId = if (inputSource == InputSource.USB) usbInputDevice()?.id ?: -1 else -1
        val usbOutputId = if (outputSource == OutputSource.USB) usbOutputDevice()?.id ?: -1 else -1
        val usbPresent = usbAudioDevice() != null
        val bluetoothOutputId = if (outputSource == OutputSource.BLUETOOTH) bluetoothOutputDevice()?.id ?: -1 else -1
        val warnings = mutableListOf<String>()
        val wifiError = displayedWifiErrorNotice
        if (!wifiError.isNullOrBlank()) warnings += wifiError
        if (wifiError.isNullOrBlank() && inputSource == InputSource.WIFI && !wifiFallbackActive && (networkRole != 2 || actualInput != -2)) warnings += "Wi-Fi 输入未生效，当前使用默认麦克风"
        if (wifiError.isNullOrBlank() && inputSource == InputSource.WIFI && wifiFallbackActive) warnings += "Wi-Fi 输入暂无数据，当前使用默认麦克风并等待恢复"
        if (inputSource == InputSource.USB && usbInputId < 0 && !usbPresent) warnings += "USB 输入已断开，当前使用系统默认输入；重新插入后将自动切回 USB"
        else if (inputSource == InputSource.USB && usbInputId < 0 && usbPresent) warnings += "当前 USB 声卡不支持输入（可能是仅输出声卡），当前使用系统默认输入"
        else if (inputSource == InputSource.USB && actualInput != -3 && actualInput != usbInputId) warnings += "USB 输入未生效，当前使用系统默认输入（实际设备 ID=$actualInput）"
        else if (inputSource == InputSource.USB && actualInputChannels < 2) warnings += "USB 输入已连接但当前仅为单声道（实际通道数=$actualInputChannels）"
        if (outputSource == OutputSource.USB && usbOutputId < 0 && !usbPresent) warnings += "USB 输出已断开，当前使用系统默认输出；重新插入后将自动切回 USB"
        else if (outputSource == OutputSource.USB && usbOutputId < 0 && usbPresent) warnings += "当前 USB 声卡不支持输出（可能是仅输入声卡），当前使用系统默认输出"
        else if (outputSource == OutputSource.USB && actualOutput != -4 && actualOutput != usbOutputId) warnings += "USB 输出未生效，当前使用系统默认输出"
        if (outputSource == OutputSource.BLUETOOTH && bluetoothOutputId < 0) warnings += "蓝牙输出已断开，当前使用系统默认输出；设备恢复后将自动重连"
        else if (outputSource == OutputSource.BLUETOOTH && actualOutput != bluetoothOutputId) warnings += "蓝牙输出未生效，正在自动重连"
        routeNotice = warnings.takeIf { it.isNotEmpty() }?.joinToString("；")
    }
    fun setRecording(enabled: Boolean): Pair<File, File>? {
        if (!enabled) {
            if (isRecording) NativeAudio.stopRecording()
            isRecording = false
            finishRecording()
            return null
        }
        val stamp = System.currentTimeMillis()
        if (Build.VERSION.SDK_INT >= 29) {
            val resolver = context.contentResolver
            try {
                val dryUri = resolver.insert(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, mediaValues("${stamp}_dry.wav")) ?: error("MediaStore insert failed")
                val wetUri = resolver.insert(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, mediaValues("${stamp}_wet.wav")) ?: error("MediaStore insert failed")
                val dryPfd = resolver.openFileDescriptor(dryUri, "w") ?: error("MediaStore open failed")
                val wetPfd = resolver.openFileDescriptor(wetUri, "w") ?: error("MediaStore open failed")
                val target = RecordingTarget(dryUri, wetUri, dryPfd, wetPfd)
                recordingTarget = target
                isRecording = NativeAudio.startRecordingFd(dryPfd.fd, wetPfd.fd)
                if (!isRecording) finishRecording(deletePending = true)
            } catch (_: Throwable) {
                recordingTarget?.let { resolver.delete(it.dryUri ?: return@let, null, null); resolver.delete(it.wetUri ?: return@let, null, null); it.dryPfd.close(); it.wetPfd.close() }
                recordingTarget = null
                lastError = "录音无法保存到系统录音目录"
            }
        } else {
            val folder = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MUSIC), "Recordings/LiveAudioProcess").apply { mkdirs() }
            runCatching {
                val dry = File(folder, "${stamp}_dry.wav"); val wet = File(folder, "${stamp}_wet.wav")
                val dryPfd = ParcelFileDescriptor.open(dry, ParcelFileDescriptor.MODE_CREATE or ParcelFileDescriptor.MODE_TRUNCATE or ParcelFileDescriptor.MODE_WRITE_ONLY)
                val wetPfd = ParcelFileDescriptor.open(wet, ParcelFileDescriptor.MODE_CREATE or ParcelFileDescriptor.MODE_TRUNCATE or ParcelFileDescriptor.MODE_WRITE_ONLY)
                recordingTarget = RecordingTarget(null, null, dryPfd, wetPfd)
                isRecording = NativeAudio.startRecordingFd(dryPfd.fd, wetPfd.fd)
                if (!isRecording) finishRecording()
            }.onFailure { lastError = "录音无法保存到系统录音目录" }
        }
        return null
    }
    private fun mediaValues(name: String) = ContentValues().apply {
        put(MediaStore.Audio.Media.DISPLAY_NAME, name)
        put(MediaStore.Audio.Media.MIME_TYPE, "audio/wav")
        put(MediaStore.Audio.Media.RELATIVE_PATH, Environment.DIRECTORY_RECORDINGS + "/LiveAudioProcess")
        put(MediaStore.Audio.Media.IS_PENDING, 1)
    }
    private fun finishRecording(deletePending: Boolean = false) {
        val target = recordingTarget ?: return
        target.dryPfd.close(); target.wetPfd.close()
        if (Build.VERSION.SDK_INT >= 29 && !deletePending) {
            target.dryUri?.let { context.contentResolver.update(it, ContentValues().apply { put(MediaStore.Audio.Media.IS_PENDING, 0) }, null, null) }
            target.wetUri?.let { context.contentResolver.update(it, ContentValues().apply { put(MediaStore.Audio.Media.IS_PENDING, 0) }, null, null) }
        } else if (deletePending && Build.VERSION.SDK_INT >= 29) {
            target.dryUri?.let { context.contentResolver.delete(it, null, null) }
            target.wetUri?.let { context.contentResolver.delete(it, null, null) }
        }
        recordingTarget = null
    }
    fun stop() { if (isRecording) setRecording(false); routeHandler.removeCallbacks(routeRestart); routeHandler.removeCallbacks(routeRefresh); routeHandler.removeCallbacks(wifiHealthMonitor); routeHandler.removeCallbacks(bluetoothRouteMonitor); NativeAudio.stop(); usbConnection?.close(); usbConnection = null; clearBluetoothRoute(); activeInputDeviceId = -1; activeOutputDeviceId = -1; usbOutputHostActive = false; observedBluetoothDeviceId = Int.MIN_VALUE; bluetoothRetryCount = 0; nextBluetoothRetryAtMs = 0L; isRecording = false; isRunning = false }
    fun refreshNativeParameters() { pushNativeParameters() }
    fun configureTone(enabled: Boolean) {
        if (NativeAudio.available) NativeAudio.configureTone(enabled, toneWaveform, toneMusic, toneChannels, toneFrequency, toneFrequency2, toneDurationSeconds, toneClickIntervalMs, toneLevel)
    }
    private fun pushNativeParameters() {
        if (!NativeAudio.available) return
        val flags = (if (dspEnabled) 1 else 0) or
            (if (eqEnabled) 2 else 0) or
            (if (reverbEnabled) 4 else 0) or
            (if (limiterEnabled) 8 else 0) or
            (if (loudnessEnabled) 16 else 0)
        NativeAudio.update(flags, floatArrayOf(
            eqFrequency, eqGain, eqQ, eq2Frequency, eq2Gain, eq2Q,
            eq3Frequency, eq3Gain, eq3Q, eq4Frequency, eq4Gain, eq4Q,
            reverbRoom, reverbDecay, reverbDamping, reverbMix * 100f,
            limiterInputGain, limiterThreshold, limiterRelease, limiterCeiling,
            limiterLookAhead, if (limiterAdaptiveRelease) 1f else 0f,
            loudnessTarget, loudnessLra, loudnessTruePeak
        ))
        NativeAudio.configureTone(inputSource == InputSource.TEST_TONE, toneWaveform, toneMusic, toneChannels, toneFrequency, toneFrequency2, toneDurationSeconds, toneClickIntervalMs, toneLevel)
    }

    companion object {
        private fun formatSampleRate(rate: Int) = if (rate % 1_000 == 0) "${rate / 1_000} kHz" else "${rate / 1000f} kHz"
        private const val BLUETOOTH_MONITOR_INTERVAL_MS = 1_000L
        private const val BLUETOOTH_ROUTE_RETRY_BASE_MS = 500L
        private const val BLUETOOTH_ROUTE_RETRY_MAX_MS = 8_000L
        private const val BLUETOOTH_ROUTE_RETRY_LIMIT = 6
    }
}

enum class InputSource(val label: String) { BUILT_IN("内置麦克风"), USB("USB 声卡 / AD2R"), WIFI("Wi-Fi 音频"), TEST_TONE("测试 Tone") }
enum class OutputSource(val label: String) { NONE("不输出"), SPEAKER("扬声器 / 有线"), USB("USB 声卡 / AD2R"), BLUETOOTH("蓝牙耳机"), WIFI("Wi-Fi 音频") }
data class ChannelPair(val index: Int, val label: String)
