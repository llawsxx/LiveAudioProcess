package com.llawsxx.audioprocess

import android.content.Context
import android.content.ContentValues
import android.content.ContentResolver
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

class AudioEngine(private val context: Context) {
    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val routeHandler = Handler(Looper.getMainLooper())
    @Volatile private var activeInputDeviceId = -1
    @Volatile private var activeOutputDeviceId = -1
    @Volatile private var bluetoothRouted = false
    private var observedBluetoothDeviceId = Int.MIN_VALUE
    private var bluetoothRetryCount = 0
    private var nextBluetoothRetryAtMs = 0L
    @Volatile private var networkRole = 0
    @Volatile private var wifiFallbackActive = false
    @Volatile var wifiInputTimeoutMs = 1_000
    @Volatile var routeNotice: String? = null
        private set
    private val deviceCallback = if (Build.VERSION.SDK_INT >= 23) object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) { scheduleRouteRefresh() }
        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) { scheduleRouteRefresh() }
    } else null

    init { if (Build.VERSION.SDK_INT >= 23) deviceCallback?.let { audioManager.registerAudioDeviceCallback(it, routeHandler) } }
    @Volatile var inputSource = InputSource.BUILT_IN
        internal set
    @Volatile var outputSource = OutputSource.SPEAKER
        internal set
    @Volatile var sampleRate = 48_000; @Volatile var bufferFrames = 256; @Volatile var inputPair = 0
    @Volatile var usbBitDepth = 16
        private set
    @Volatile var usbInputBurstPackets = 8
        private set
    @Volatile var usbOutputBurstPackets = 8
        private set
    @Volatile var eqGain = 2f; @Volatile var eqFrequency = 1200f; @Volatile var eqQ = .85f
    @Volatile var eq2Frequency = 250f; @Volatile var eq2Gain = 0f; @Volatile var eq2Q = 1f
    @Volatile var eq3Frequency = 4000f; @Volatile var eq3Gain = 0f; @Volatile var eq3Q = 1f
    @Volatile var eq4Frequency = 10000f; @Volatile var eq4Gain = 0f; @Volatile var eq4Q = 1f
    @Volatile var reverbRoom = 42f; @Volatile var reverbDecay = 1.8f; @Volatile var reverbDamping = 35f; @Volatile var reverbMix = .18f
    @Volatile var limiterInputGain = 0f; @Volatile var limiterThreshold = -.5f; @Volatile var limiterRelease = 80f; @Volatile var limiterCeiling = -.5f; @Volatile var limiterLookAhead = 1f; @Volatile var limiterAdaptiveRelease = false
    @Volatile var dspEnabled = true; @Volatile var eqEnabled = true; @Volatile var reverbEnabled = true; @Volatile var limiterEnabled = true
    @Volatile var isRunning = false; private set
    @Volatile var isRecording = false; private set
    @Volatile var lastError: String? = null; private set
    private data class RecordingTarget(val dryUri: android.net.Uri?, val wetUri: android.net.Uri?, val dryPfd: ParcelFileDescriptor, val wetPfd: ParcelFileDescriptor)
    private var recordingTarget: RecordingTarget? = null
    private var usbConnection: UsbDeviceConnection? = null
    fun configureNetwork(role: Int, codec: Int, host: String, port: Int, minBufferMs: Int, maxBufferMs: Int): Boolean {
        val configured = NativeAudio.configureNetwork(role, codec, host, port, minBufferMs, maxBufferMs)
        networkRole = if (configured) role else 0
        if (!configured) routeNotice = "Wi-Fi 音频配置失败，所选 Wi-Fi 路由未生效"
        return configured
    }
    fun clearNetwork() {
        NativeAudio.clearNetwork()
        networkRole = 0
        wifiFallbackActive = false
    }
    fun configureUsbOutputBuffer(minBufferMs: Int, maxBufferMs: Int) {
        if (!NativeAudio.available) return
        val minMs = minBufferMs.coerceIn(8, 200)
        val maxMs = maxBufferMs.coerceIn(8, 500).coerceAtLeast(minMs)
        NativeAudio.configureUsbOutputBuffer(minMs, maxMs)
    }
    fun configureAudioFormat(requestedSampleRate: Int, requestedUsbBitDepth: Int) {
        val normalizedRate = requestedSampleRate.takeIf { it == 44_100 || it == 48_000 || it == 96_000 } ?: 48_000
        val normalizedBitDepth = requestedUsbBitDepth.takeIf { it == 16 || it == 24 || it == 32 } ?: 16
        val rateChanged = sampleRate != normalizedRate
        val bitDepthChanged = usbBitDepth != normalizedBitDepth
        sampleRate = normalizedRate
        usbBitDepth = normalizedBitDepth
        if (isRunning && (rateChanged || (bitDepthChanged && (inputSource == InputSource.USB || outputSource == OutputSource.USB)))) {
            routeNotice = "正在应用新的音频格式"
            routeHandler.removeCallbacks(routeRestart)
            routeHandler.post(routeRestart)
        }
    }
    fun configureUsbBursts(inputPackets: Int, outputPackets: Int) {
        fun normalize(value: Int) = value.takeIf { it == 1 || it == 2 || it == 4 || it == 8 || it == 16 } ?: 8
        val normalizedInput = normalize(inputPackets)
        val normalizedOutput = normalize(outputPackets)
        if (usbInputBurstPackets == normalizedInput && usbOutputBurstPackets == normalizedOutput) return
        usbInputBurstPackets = normalizedInput
        usbOutputBurstPackets = normalizedOutput
        if (isRunning && (inputSource == InputSource.USB || outputSource == OutputSource.USB)) {
            restartStreamsForRouteChange()
        }
    }
    private fun usbInputDevice(): AudioDeviceInfo? {
        return audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS).firstOrNull { it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET }
    }
    private fun usbOutputDevice(): AudioDeviceInfo? {
        return audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).firstOrNull { it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET }
    }
    private fun openUsbHostConnection(): Int {
        usbConnection?.close()
        usbConnection = null
        val manager = context.getSystemService(Context.USB_SERVICE) as UsbManager
        val device = manager.deviceList.values.firstOrNull { usbDevice ->
            (0 until usbDevice.interfaceCount).any { usbDevice.getInterface(it).interfaceClass == android.hardware.usb.UsbConstants.USB_CLASS_AUDIO }
        } ?: return -1
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
            if (inputSource == InputSource.WIFI && networkRole == 2) {
                val timedOut = NativeAudio.networkInputTimedOut(wifiInputTimeoutMs.coerceIn(100, 60_000))
                if (timedOut && !wifiFallbackActive) {
                    wifiFallbackActive = true
                    restartStreamsForRouteChange()
                    routeNotice = "Wi-Fi 输入超过 ${wifiInputTimeoutMs} ms 无数据，已自动切换到默认麦克风"
                    return
                }
                if (!timedOut && wifiFallbackActive) {
                    wifiFallbackActive = false
                    restartStreamsForRouteChange()
                    routeNotice = "Wi-Fi 输入已恢复，已自动切回 Wi-Fi 音频"
                    return
                }
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
        NativeAudio.stop()
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
        if (!NativeAudio.available) { lastError = "AAudio requires Android 8.0 or newer"; return }
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
        if (inputSource == InputSource.USB && inputDeviceId < 0) lastError = "USB 输入已断开，暂使用默认麦克风"
        if (outputSource == OutputSource.USB && outputDeviceId < 0) lastError = "USB 输出已断开，暂使用默认扬声器"
        if (outputSource == OutputSource.BLUETOOTH && outputDeviceId < 0) lastError = "未检测到蓝牙耳机，暂使用默认输出"
        val useNetworkInput = inputSource == InputSource.WIFI && networkRole == 2 && !wifiFallbackActive
        val requestedUsbInputHost = inputSource == InputSource.USB
        val requestedUsbOutputHost = outputSource == OutputSource.USB
        val usbFd = if (requestedUsbInputHost || requestedUsbOutputHost) openUsbHostConnection() else -1
        val usbInputHost = requestedUsbInputHost && usbFd >= 0
        val usbOutputHost = requestedUsbOutputHost && usbFd >= 0
        var started = NativeAudio.start(sampleRate, bufferFrames, inputDeviceId, outputDeviceId, nativeInputChannels, inputPair, useNetworkInput, usbFd, usbInputHost, usbOutputHost, usbBitDepth, usbInputBurstPackets, usbOutputBurstPackets)
        if (!started && usbFd >= 0) {
            usbConnection?.close()
            usbConnection = null
            lastError = "USB Host 不支持 ${formatSampleRate(sampleRate)} / ${usbBitDepth}-bit，正在回退到系统 USB 音频"
            started = NativeAudio.start(sampleRate, bufferFrames, inputDeviceId, outputDeviceId, nativeInputChannels, inputPair, useNetworkInput, -1, false, false, usbBitDepth, usbInputBurstPackets, usbOutputBurstPackets)
        }
        if (!started && inputSource == InputSource.USB && nativeInputChannels == 2) {
            // Keep USB usable on devices whose driver rejects a stereo AAudio request.
            started = NativeAudio.start(sampleRate, bufferFrames, inputDeviceId, outputDeviceId, 1, inputPair, useNetworkInput, -1, false, false, usbBitDepth, usbInputBurstPackets, usbOutputBurstPackets)
            if (started) lastError = "USB 输入驱动拒绝立体声，已回退为单声道"
        }
        if (started) isRunning = true
        else { clearBluetoothRoute(); lastError = "AAudio stream open failed; check microphone and speaker settings" }
        if (isRunning) {
            activeInputDeviceId = inputDeviceId
            activeOutputDeviceId = outputDeviceId
            refreshRouteNotice()
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
        val bluetoothOutputId = if (outputSource == OutputSource.BLUETOOTH) bluetoothOutputDevice()?.id ?: -1 else -1
        val warnings = mutableListOf<String>()
        if (inputSource == InputSource.WIFI && !wifiFallbackActive && (networkRole != 2 || actualInput != -2)) warnings += "Wi-Fi 输入未生效，当前使用默认麦克风"
        if (inputSource == InputSource.WIFI && wifiFallbackActive) warnings += "Wi-Fi 输入暂无数据，当前使用默认麦克风并等待恢复"
        if (inputSource == InputSource.USB && actualInput != -3 && (usbInputId < 0 || actualInput != usbInputId)) warnings += "USB 输入未生效，当前使用系统默认输入（实际设备 ID=$actualInput）"
        else if (inputSource == InputSource.USB && actualInputChannels < 2) warnings += "USB 输入已连接但当前仅为单声道（实际通道数=$actualInputChannels）"
        if (outputSource == OutputSource.USB && (usbOutputId < 0 || (actualOutput != -4 && actualOutput != usbOutputId))) warnings += "USB 输出未生效，当前使用系统默认输出"
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
    fun stop() { if (isRecording) setRecording(false); routeHandler.removeCallbacks(routeRestart); routeHandler.removeCallbacks(routeRefresh); routeHandler.removeCallbacks(wifiHealthMonitor); routeHandler.removeCallbacks(bluetoothRouteMonitor); NativeAudio.stop(); usbConnection?.close(); usbConnection = null; clearBluetoothRoute(); activeInputDeviceId = -1; activeOutputDeviceId = -1; observedBluetoothDeviceId = Int.MIN_VALUE; bluetoothRetryCount = 0; nextBluetoothRetryAtMs = 0L; isRecording = false; isRunning = false }
    fun refreshNativeParameters() { pushNativeParameters() }
    private fun pushNativeParameters() { if (NativeAudio.available) NativeAudio.update((if (dspEnabled) 1 else 0) or (if (eqEnabled) 2 else 0) or (if (reverbEnabled) 4 else 0) or (if (limiterEnabled) 8 else 0), floatArrayOf(eqFrequency, eqGain, eqQ, eq2Frequency, eq2Gain, eq2Q, eq3Frequency, eq3Gain, eq3Q, eq4Frequency, eq4Gain, eq4Q, reverbRoom, reverbDecay, reverbDamping, reverbMix * 100f, limiterInputGain, limiterThreshold, limiterRelease, limiterCeiling, limiterLookAhead, if (limiterAdaptiveRelease) 1f else 0f)) }

    companion object {
        private fun formatSampleRate(rate: Int) = if (rate % 1_000 == 0) "${rate / 1_000} kHz" else "${rate / 1000f} kHz"
        private const val BLUETOOTH_MONITOR_INTERVAL_MS = 1_000L
        private const val BLUETOOTH_ROUTE_RETRY_BASE_MS = 500L
        private const val BLUETOOTH_ROUTE_RETRY_MAX_MS = 8_000L
        private const val BLUETOOTH_ROUTE_RETRY_LIMIT = 6
    }
}

enum class InputSource(val label: String) { BUILT_IN("内置麦克风"), USB("USB 声卡 / AD2R"), WIFI("Wi-Fi 音频") }
enum class OutputSource(val label: String) { SPEAKER("扬声器 / 有线"), USB("USB 声卡 / AD2R"), BLUETOOTH("蓝牙耳机"), WIFI("Wi-Fi 音频") }
data class ChannelPair(val index: Int, val label: String)
