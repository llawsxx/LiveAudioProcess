package com.llawsxx.audioprocess

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat

object AudioEngineStore {
    @Volatile private var shared: AudioEngine? = null
    fun get(context: Context): AudioEngine = synchronized(this) {
        shared ?: AudioEngine(context.applicationContext).also { shared = it }
    }
}

class AudioProcessingService : Service() {
    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForeground(NOTIFICATION_ID, notification())
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val engine = AudioEngineStore.get(this)
        if (intent?.action == ACTION_STOP) {
            engine.stop()
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        } else if (!engine.isRunning) {
            restoreSettings(engine)
            engine.start()
            if (!engine.isRunning) {
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf(startId)
                return START_NOT_STICKY
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        AudioEngineStore.get(this).stop()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "PulseForge 音频引擎", NotificationManager.IMPORTANCE_LOW)
            )
        }
    }

    private fun notification(): Notification = NotificationCompat.Builder(this, CHANNEL_ID)
        .setSmallIcon(android.R.drawable.ic_btn_speak_now)
        .setContentTitle("PulseForge 正在监听")
        .setContentText("实时 DSP 音频处理运行中")
        .setOngoing(true)
        .setCategory(NotificationCompat.CATEGORY_SERVICE)
        .build()

    private fun restoreSettings(engine: AudioEngine) {
        val p = getSharedPreferences("pulseforge_settings", MODE_PRIVATE)
        val effects = EffectSettings.load(p)
        engine.dspEnabled = effects.dspEnabled; engine.eqEnabled = effects.eqEnabled; engine.reverbEnabled = effects.reverbEnabled; engine.limiterEnabled = effects.limiterEnabled
        engine.eqFrequency = effects.eqFrequency; engine.eqGain = effects.eqGain; engine.eqQ = effects.eqQ
        engine.eq2Frequency = effects.eq2Frequency; engine.eq2Gain = effects.eq2Gain; engine.eq2Q = effects.eq2Q
        engine.eq3Frequency = effects.eq3Frequency; engine.eq3Gain = effects.eq3Gain; engine.eq3Q = effects.eq3Q
        engine.eq4Frequency = effects.eq4Frequency; engine.eq4Gain = effects.eq4Gain; engine.eq4Q = effects.eq4Q
        engine.reverbRoom = effects.reverbRoom; engine.reverbDecay = effects.reverbDecay; engine.reverbDamping = effects.reverbDamping; engine.reverbMix = effects.reverbMix / 100f
        engine.limiterInputGain = effects.limiterInputGain; engine.limiterThreshold = effects.limiterThreshold; engine.limiterRelease = effects.limiterRelease; engine.limiterCeiling = effects.limiterCeiling; engine.limiterLookAhead = effects.limiterLookAhead
        engine.sampleRate = p.getInt("rate", 48_000); engine.bufferFrames = p.getInt("buffer", 256)
        engine.wifiInputTimeoutMs = (((p.getString("wifiInputTimeout", "1.0")?.toFloatOrNull() ?: 1f) * 1000f).toInt()).coerceIn(100, 60_000)
        engine.inputPair = p.getInt("channelPair", 0)
        engine.inputSource = runCatching { InputSource.valueOf(p.getString("input", InputSource.BUILT_IN.name)!!) }.getOrDefault(InputSource.BUILT_IN)
        engine.outputSource = runCatching { OutputSource.valueOf(p.getString("output", OutputSource.SPEAKER.name)!!) }.getOrDefault(OutputSource.SPEAKER)
        val wifiOutputEnabled = p.getBoolean("wifiOutputEnabled", false)
        val wifiRole = if (engine.inputSource == InputSource.WIFI) 2 else if (wifiOutputEnabled) 1 else 0
        if (wifiRole != 0) {
            val host = p.getString("wifiHost", "192.168.1.2") ?: "192.168.1.2"
            val port = p.getString("wifiPort", "40100")?.toIntOrNull() ?: 40100
            val minBuffer = (p.getString("wifiMinBuffer", "50")?.toIntOrNull() ?: 50).coerceIn(0, 200)
            val maxBuffer = (p.getString("wifiMaxBuffer", "100")?.toIntOrNull() ?: 100).coerceIn(50, 1000)
            engine.configureNetwork(wifiRole, 0, host, port, minBuffer, maxBuffer)
        }
    }

    companion object {
        const val ACTION_START = "com.llawsxx.audioprocess.START"
        const val ACTION_STOP = "com.llawsxx.audioprocess.STOP"
        private const val CHANNEL_ID = "pulseforge_audio"
        private const val NOTIFICATION_ID = 42
    }
}
