package com.llawsxx.audioprocess

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat

object AudioEngineStore {
    @Volatile private var shared: AudioEngine? = null
    fun get(context: Context): AudioEngine = synchronized(this) {
        shared ?: AudioEngine(context.applicationContext).also { shared = it }
    }
}

class AudioProcessingService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForeground(NOTIFICATION_ID, notification())
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val engine = AudioEngineStore.get(this)
        if (intent?.action == ACTION_STOP) {
            engine.stop()
            releaseWakeLock()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        } else if (!engine.isRunning) {
            restoreSettings(engine)
            engine.start()
            if (!engine.isRunning) {
                releaseWakeLock()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf(startId)
                return START_NOT_STICKY
            }
        }
        if (engine.isRunning) acquireWakeLock()
        return START_STICKY
    }

    override fun onDestroy() {
        AudioEngineStore.get(this).stop()
        releaseWakeLock()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun acquireWakeLock() {
        val lock = wakeLock ?: (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "$packageName:AudioProcessing")
            .also {
                it.setReferenceCounted(false)
                wakeLock = it
            }
        if (!lock.isHeld) lock.acquire()
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "LiveAudioProcess 音频引擎", NotificationManager.IMPORTANCE_LOW)
            )
        }
    }

    private fun notification(): Notification = NotificationCompat.Builder(this, CHANNEL_ID)
        .setSmallIcon(android.R.drawable.ic_btn_speak_now)
        .setContentTitle("LiveAudioProcess 正在监听")
        .setContentText("实时 DSP 音频处理运行中")
        .setOngoing(true)
        .setCategory(NotificationCompat.CATEGORY_SERVICE)
        .build()

    private fun restoreSettings(engine: AudioEngine) {
        val p = getSharedPreferences("audioprocess_settings", MODE_PRIVATE)
        val effects = EffectSettings.load(p)
        engine.dspEnabled = effects.dspEnabled; engine.eqEnabled = effects.eqEnabled; engine.reverbEnabled = effects.reverbEnabled; engine.limiterEnabled = effects.limiterEnabled
        engine.eqFrequency = effects.eqFrequency; engine.eqGain = effects.eqGain; engine.eqQ = effects.eqQ
        engine.eq2Frequency = effects.eq2Frequency; engine.eq2Gain = effects.eq2Gain; engine.eq2Q = effects.eq2Q
        engine.eq3Frequency = effects.eq3Frequency; engine.eq3Gain = effects.eq3Gain; engine.eq3Q = effects.eq3Q
        engine.eq4Frequency = effects.eq4Frequency; engine.eq4Gain = effects.eq4Gain; engine.eq4Q = effects.eq4Q
        engine.reverbRoom = effects.reverbRoom; engine.reverbDecay = effects.reverbDecay; engine.reverbDamping = effects.reverbDamping; engine.reverbMix = effects.reverbMix / 100f
        engine.limiterInputGain = effects.limiterInputGain; engine.limiterThreshold = effects.limiterThreshold; engine.limiterRelease = effects.limiterRelease; engine.limiterCeiling = effects.limiterCeiling; engine.limiterLookAhead = effects.limiterLookAhead; engine.limiterAdaptiveRelease = effects.limiterAdaptiveRelease
        engine.loudnessTarget = effects.loudnessTarget; engine.loudnessLra = effects.loudnessLra; engine.loudnessTruePeak = effects.loudnessTruePeak; engine.loudnessEnabled = effects.loudnessEnabled
        engine.toneWaveform = p.getInt("toneWaveform", 0).coerceIn(0, 7)
        engine.toneMusic = p.getInt("toneMusic", 0).coerceIn(0, 5)
        engine.toneChannels = p.getInt("toneChannels", 0).coerceIn(0, 2)
        engine.toneFrequency = p.getFloat("toneFrequency", 1000f).coerceIn(1f, 20_000f)
        engine.toneFrequency2 = p.getFloat("toneFrequency2", 20_000f).coerceIn(1f, 20_000f)
        engine.toneDurationSeconds = p.getFloat("toneDurationSeconds", 10f).coerceIn(1f, 60f)
        engine.toneClickIntervalMs = p.getFloat("toneClickIntervalMs", 1000f).coerceIn(50f, 5000f)
        engine.toneLevel = Math.pow(10.0, p.getInt("toneLevelDb", -12).coerceIn(-60, 0).toDouble() / 20.0).toFloat()
        val legacyRate = p.getInt("rate", 48_000)
        val legacyBits = p.getInt("usbBitDepth", 16)
        engine.configureAudioFormat(
            legacyRate,
            p.getInt("outputRate", p.getInt("usbOutputRate", legacyRate)),
            p.getInt("usbInputBitDepth", legacyBits), p.getInt("usbOutputBitDepth", legacyBits)
        ); engine.bufferFrames = p.getInt("buffer", 256)
        val outputBufferMaxMs = if (p.contains("systemOutputBufferMaxMs")) {
            p.getInt("systemOutputBufferMaxMs", 40)
        } else {
            if (p.contains("systemOutputBufferBursts")) {
                (p.getInt("systemOutputBufferBursts", 4) * 2).coerceIn(5, 200)
            } else 40
        }
        engine.configureSystemOutputBuffer(outputBufferMaxMs)
        engine.configureSystemInputBuffer(p.getInt("systemInputBufferMaxMs", 20))
        engine.configureUsbOutputBuffer(p.getInt("usbMaxBuffer", 50))
        engine.configureUsbInputBuffer(p.getInt("usbInputBufferMaxMs", 20))
        val legacyUsbBurst = p.getInt("usbBurstPackets", 8)
        engine.configureUsbBursts(
            p.getInt("usbInputBurstPackets", legacyUsbBurst),
            p.getInt("usbOutputBurstPackets", legacyUsbBurst)
        )
        engine.wifiInputTimeoutMs = (((p.getString("wifiInputTimeout", "1.0")?.toFloatOrNull() ?: 1f) * 1000f).toInt()).coerceIn(100, 60_000)
        engine.inputPair = p.getInt("channelPair", 0)
        engine.inputSource = runCatching { InputSource.valueOf(p.getString("input", InputSource.BUILT_IN.name)!!) }.getOrDefault(InputSource.BUILT_IN)
        engine.outputSource = runCatching { OutputSource.valueOf(p.getString("output", OutputSource.SPEAKER.name)!!) }.getOrDefault(OutputSource.SPEAKER)
        engine.outputVolumePercent = if (engine.outputSource == OutputSource.USB) {
            p.getInt("usbOutputVolumePercent", p.getInt("outputVolumePercent", 100)).coerceIn(0, 100)
        } else engine.systemVolumePercent()
        val wifiOutputEnabled = p.getBoolean("wifiOutputEnabled", false)
        val wifiRole = if (engine.inputSource == InputSource.WIFI) 2 else if (wifiOutputEnabled) 1 else 0
        if (wifiRole != 0) {
            val legacyHost = p.getString("wifiHost", "192.168.1.2") ?: "192.168.1.2"
            val legacyPort = p.getString("wifiPort", "40100") ?: "40100"
            val host = if (wifiRole == 2) p.getString("wifiReceiveHost", legacyHost) ?: legacyHost else p.getString("wifiSendHost", legacyHost) ?: legacyHost
            val port = if (wifiRole == 2) p.getString("wifiReceivePort", legacyPort)?.toIntOrNull() ?: 40100 else p.getString("wifiSendPort", legacyPort)?.toIntOrNull() ?: 40100
            val minBuffer = (p.getString("wifiMinBuffer", "0")?.toIntOrNull() ?: 0).coerceIn(0, 200)
            val maxBuffer = (p.getString("wifiMaxBuffer", "200")?.toIntOrNull() ?: 200).coerceIn(50, 1000)
            val maxHold = p.getInt("wifiMaxHoldMs", 1000).coerceIn(0, 60_000)
            val packetDuration = runCatching { p.getInt("wifiPacketDurationMs", 20) }
                .getOrElse { p.getString("wifiPacketDurationMs", "20")?.toIntOrNull() ?: 20 }
                .coerceIn(1, 100)
            engine.wifiClockCorrectionEnabled = p.getBoolean("wifiClockCorrectionEnabled", false)
            engine.wifiLowLatencyEnabled = p.getBoolean("wifiLowLatencyEnabled", false)
            engine.wifiQosEnabled = p.getBoolean("wifiQosEnabled", false)
            engine.wifiRetransmitEnabled = p.getBoolean("wifiRetransmitEnabled", true)
            engine.wifiOpusFrameMs = p.getInt("wifiOpusFrameMs", 20).let { if (it in listOf(5, 10, 20, 40, 60)) it else 20 }
            engine.wifiOpusProfile = p.getInt("wifiOpusProfile", 0).coerceIn(0, 2)
            engine.wifiDynamicBufferEnabled = p.getBoolean("wifiDynamicBufferEnabled", true)
            engine.wifiManualBufferBias = (p.getInt("wifiManualBufferBiasPermille", 500).coerceIn(0, 1000) / 1000f)
            engine.configureNetwork(wifiRole, p.getInt("wifiTransport", 0).coerceIn(0, 1), p.getInt("wifiCodec", 0).coerceIn(0, 2), p.getInt("wifiAacBitrate", 128_000), host, port, packetDuration, minBuffer, maxBuffer, maxHold)
        }
    }

    companion object {
        const val ACTION_START = "com.llawsxx.audioprocess.START"
        const val ACTION_STOP = "com.llawsxx.audioprocess.STOP"
        private const val CHANNEL_ID = "audioprocess_audio"
        private const val NOTIFICATION_ID = 42
    }
}
