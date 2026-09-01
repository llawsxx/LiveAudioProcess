package com.llawsxx.audioprocess

import android.Manifest
import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.os.Build
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.llawsxx.audioprocess.ui.theme.LiveAudioProcessTheme
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {
    private val usbPermissionAction = "com.llawsxx.audioprocess.USB_AUDIO_PERMISSION"
    private var usbPermissionCallback: ((Boolean) -> Unit)? = null
    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != usbPermissionAction) return
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            usbPermissionCallback?.invoke(granted)
            usbPermissionCallback = null
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val filter = IntentFilter(usbPermissionAction)
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(usbPermissionReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(usbPermissionReceiver, filter)
        }
        setContent { LiveAudioProcessTheme { LiveAudioProcessApp() } }
    }

    override fun onDestroy() {
        usbPermissionCallback = null
        runCatching { unregisterReceiver(usbPermissionReceiver) }
        super.onDestroy()
    }

    fun requestUsbAudioPermission(onResult: (Boolean) -> Unit) {
        val usbManager = getSystemService(Context.USB_SERVICE) as UsbManager
        val device = usbManager.deviceList.values.firstOrNull { usbDevice ->
            (0 until usbDevice.interfaceCount).any {
                usbDevice.getInterface(it).interfaceClass == UsbConstants.USB_CLASS_AUDIO
            }
        }
        if (device == null || usbManager.hasPermission(device)) {
            onResult(true)
            return
        }
        usbPermissionCallback = onResult
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
            if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        val permissionIntent = PendingIntent.getBroadcast(
            this, 0, Intent(usbPermissionAction).setPackage(packageName), flags
        )
        usbManager.requestPermission(device, permissionIntent)
    }
}

private val Ink = Color(0xFF101417)
private val Panel = Color(0xFF182126)
private val PanelRaised = Color(0xFF202D33)
private val Muted = Color(0xFF8EA0A8)
private val Teal = Color(0xFF43D5C1)
private val Amber = Color(0xFFFFC857)
private val Red = Color(0xFFFF6B6B)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LiveAudioProcessApp() {
    val context = LocalContext.current
    val activity = context as? Activity
    val engine = remember { AudioEngineStore.get(context.applicationContext) }
    val prefs = remember { context.getSharedPreferences("pulseforge_settings", android.content.Context.MODE_PRIVATE) }
    var screenAlwaysOn by remember { mutableStateOf(prefs.getBoolean("screenAlwaysOn", false)) }
    var running by remember { mutableStateOf(false) }
    var recording by remember { mutableStateOf(false) }
    var input by remember { mutableStateOf(runCatching { InputSource.valueOf(prefs.getString("input", InputSource.BUILT_IN.name)!!) }.getOrDefault(InputSource.BUILT_IN)) }
    var channelPair by remember { mutableIntStateOf(prefs.getInt("channelPair", 0)) }
    var output by remember { mutableStateOf(runCatching { OutputSource.valueOf(prefs.getString("output", OutputSource.SPEAKER.name)!!) }.getOrDefault(OutputSource.SPEAKER).let { if (it == OutputSource.WIFI) OutputSource.SPEAKER else it }) }
    var wifiOutputEnabled by remember { mutableStateOf(input != InputSource.WIFI && prefs.getBoolean("wifiOutputEnabled", prefs.getString("output", "") == OutputSource.WIFI.name)) }
    var rate by remember { mutableIntStateOf(prefs.getInt("rate", 48_000)) }
    var buffer by remember { mutableIntStateOf(prefs.getInt("buffer", 256)) }
    var inputLevelL by remember { mutableFloatStateOf(0.08f) }; var inputLevelR by remember { mutableFloatStateOf(0.08f) }
    var outputLevelL by remember { mutableFloatStateOf(0.05f) }; var outputLevelR by remember { mutableFloatStateOf(0.05f) }
    var effects by remember { mutableStateOf(EffectSettings.load(prefs)) }
    var limiterGain by remember { mutableFloatStateOf(1f) }
    var limiterReleaseMs by remember { mutableFloatStateOf(effects.limiterRelease) }
    val legacyWifiHost = prefs.getString("wifiHost", "192.168.1.2") ?: "192.168.1.2"
    val legacyWifiPort = prefs.getString("wifiPort", "40100") ?: "40100"
    var wifiSendHost by remember { mutableStateOf(prefs.getString("wifiSendHost", legacyWifiHost) ?: legacyWifiHost) }
    var wifiSendPort by remember { mutableStateOf(prefs.getString("wifiSendPort", legacyWifiPort) ?: legacyWifiPort) }
    var wifiReceiveHost by remember { mutableStateOf(prefs.getString("wifiReceiveHost", if (input == InputSource.WIFI) legacyWifiHost else "0.0.0.0") ?: "0.0.0.0") }
    var wifiReceivePort by remember { mutableStateOf(prefs.getString("wifiReceivePort", legacyWifiPort) ?: legacyWifiPort) }
    var wifiMinBuffer by remember { mutableStateOf(prefs.getString("wifiMinBuffer", "50") ?: "50") }; var wifiMaxBuffer by remember { mutableStateOf(prefs.getString("wifiMaxBuffer", "100") ?: "100") }; var wifiActive by remember { mutableStateOf(false) }
    var wifiInputTimeout by remember { mutableStateOf(prefs.getString("wifiInputTimeout", "1.0") ?: "1.0") }
    var usbMinBuffer by remember { mutableStateOf(prefs.getInt("usbMinBuffer", 16).toString()) }
    var usbMaxBuffer by remember { mutableStateOf(prefs.getInt("usbMaxBuffer", 50).toString()) }
    var usbBitDepth by remember {
        mutableIntStateOf(prefs.getInt("usbBitDepth", 16).takeIf { it == 16 || it == 24 || it == 32 } ?: 16)
    }
    val legacyUsbBurstPackets = prefs.getInt("usbBurstPackets", 8)
    var usbInputBurstPackets by remember {
        mutableIntStateOf(prefs.getInt("usbInputBurstPackets", legacyUsbBurstPackets).takeIf { it == 1 || it == 2 || it == 4 || it == 8 || it == 16 } ?: 8)
    }
    var usbOutputBurstPackets by remember {
        mutableIntStateOf(prefs.getInt("usbOutputBurstPackets", legacyUsbBurstPackets).takeIf { it == 1 || it == 2 || it == 4 || it == 8 || it == 16 } ?: 8)
    }
    var usbStats by remember { mutableStateOf(LongArray(12)) }
    var routeNotice by remember { mutableStateOf<String?>(null) }
    var elapsed by remember { mutableIntStateOf(0) }
    var hasPermission by remember { mutableStateOf(ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) }
    var pendingBluetoothOutput by remember { mutableStateOf<OutputSource?>(null) }
    val channelPairs = remember(input) { if (input == InputSource.USB) engine.availableInputPairs() else listOf(ChannelPair(0, "Mono / Mic")) }
    val permissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { hasPermission = it }
    val bluetoothPermissionLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted -> if (granted) pendingBluetoothOutput?.let { output = it }; pendingBluetoothOutput = null }
    DisposableEffect(activity, screenAlwaysOn) {
        activity?.window?.let { window ->
            if (screenAlwaysOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
        onDispose {
            if (screenAlwaysOn) activity?.window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }
    DisposableEffect(engine) { onDispose { } }
    LaunchedEffect(Unit) {
        while (true) {
            running = engine.isRunning
            routeNotice = engine.routeNotice ?: engine.lastError
            delay(100)
        }
    }
    LaunchedEffect(recording) { while (recording) { delay(1000); elapsed++ } }
    LaunchedEffect(running) {
        while (running && NativeAudio.available) {
            val levels = NativeAudio.levels(); if (levels.size >= 4) { inputLevelL = levels[0]; inputLevelR = levels[1]; outputLevelL = levels[2]; outputLevelR = levels[3] }; if (levels.size >= 6) { limiterGain = levels[4]; limiterReleaseMs = levels[5] }; delay(50)
        }
    }
    LaunchedEffect(running, input, output) {
        if (!running || (input != InputSource.USB && output != OutputSource.USB)) {
            usbStats = LongArray(12)
            return@LaunchedEffect
        }
        while (running && NativeAudio.available) {
            usbStats = NativeAudio.usbStats()
            delay(500)
        }
    }
    fun configureWifiForCurrentRoute(): Boolean = when {
        input == InputSource.WIFI -> engine.configureNetwork(2, 0, wifiReceiveHost, wifiReceivePort.toIntOrNull() ?: 40100, wifiMinBuffer.toIntOrNull()?.coerceIn(0, 200) ?: 50, wifiMaxBuffer.toIntOrNull()?.coerceIn(50, 1000) ?: 100)
        wifiOutputEnabled -> engine.configureNetwork(1, 0, wifiSendHost, wifiSendPort.toIntOrNull() ?: 40100, 0, 50)
        else -> { engine.clearNetwork(); false }
    }
    fun syncEngine() { engine.configureAudioFormat(rate, usbBitDepth); engine.bufferFrames = buffer; engine.eqGain = effects.eqGain; engine.eqFrequency = effects.eqFrequency; engine.eqQ = effects.eqQ; engine.eq2Frequency = effects.eq2Frequency; engine.eq2Gain = effects.eq2Gain; engine.eq2Q = effects.eq2Q; engine.eq3Frequency = effects.eq3Frequency; engine.eq3Gain = effects.eq3Gain; engine.eq3Q = effects.eq3Q; engine.eq4Frequency = effects.eq4Frequency; engine.eq4Gain = effects.eq4Gain; engine.eq4Q = effects.eq4Q; engine.reverbRoom = effects.reverbRoom; engine.reverbDecay = effects.reverbDecay; engine.reverbDamping = effects.reverbDamping; engine.reverbMix = effects.reverbMix / 100f; engine.limiterInputGain = effects.limiterInputGain; engine.limiterThreshold = effects.limiterThreshold; engine.limiterRelease = effects.limiterRelease; engine.limiterCeiling = effects.limiterCeiling; engine.limiterLookAhead = effects.limiterLookAhead; engine.limiterAdaptiveRelease = effects.limiterAdaptiveRelease; engine.wifiInputTimeoutMs = ((wifiInputTimeout.toFloatOrNull() ?: 1f) * 1000f).toInt().coerceIn(100, 60_000); engine.updateRouting(input, output, channelPair); effects.save(prefs); prefs.edit().putInt("rate", rate).putInt("usbBitDepth", usbBitDepth).putInt("buffer", buffer).putInt("channelPair", channelPair).putString("input", input.name).putString("output", output.name).putBoolean("wifiOutputEnabled", wifiOutputEnabled).putBoolean("wifiActive", wifiActive).apply() }
    LaunchedEffect(input, channelPair, output, wifiOutputEnabled, rate, usbBitDepth, buffer, effects) { syncEngine(); engine.dspEnabled = effects.dspEnabled; engine.eqEnabled = effects.eqEnabled; engine.reverbEnabled = effects.reverbEnabled; engine.limiterEnabled = effects.limiterEnabled; engine.limiterInputGain = effects.limiterInputGain; if (input == InputSource.WIFI || wifiOutputEnabled) wifiActive = configureWifiForCurrentRoute() else { engine.clearNetwork(); wifiActive = false }; engine.refreshNativeParameters() }
    LaunchedEffect(wifiSendHost, wifiSendPort) {
        prefs.edit().putString("wifiSendHost", wifiSendHost).putString("wifiSendPort", wifiSendPort).apply()
        if (wifiOutputEnabled && input != InputSource.WIFI) wifiActive = configureWifiForCurrentRoute()
    }
    LaunchedEffect(wifiReceiveHost, wifiReceivePort, wifiMinBuffer, wifiMaxBuffer, wifiInputTimeout) {
        engine.wifiInputTimeoutMs = ((wifiInputTimeout.toFloatOrNull() ?: 1f) * 1000f).toInt().coerceIn(100, 60_000)
        prefs.edit().putString("wifiReceiveHost", wifiReceiveHost).putString("wifiReceivePort", wifiReceivePort).putString("wifiMinBuffer", wifiMinBuffer).putString("wifiMaxBuffer", wifiMaxBuffer).putString("wifiInputTimeout", wifiInputTimeout).apply()
        if (input == InputSource.WIFI) wifiActive = configureWifiForCurrentRoute()
    }
    LaunchedEffect(usbMinBuffer, usbMaxBuffer) {
        val minMs = usbMinBuffer.toIntOrNull() ?: return@LaunchedEffect
        val maxMs = usbMaxBuffer.toIntOrNull() ?: return@LaunchedEffect
        delay(400)
        val appliedMinMs = minMs.coerceIn(8, 200)
        val appliedMaxMs = maxMs.coerceIn(8, 500).coerceAtLeast(appliedMinMs)
        prefs.edit().putInt("usbMinBuffer", appliedMinMs).putInt("usbMaxBuffer", appliedMaxMs).apply()
        engine.configureUsbOutputBuffer(appliedMinMs, appliedMaxMs)
    }
    LaunchedEffect(usbInputBurstPackets, usbOutputBurstPackets) {
        prefs.edit()
            .putInt("usbInputBurstPackets", usbInputBurstPackets)
            .putInt("usbOutputBurstPackets", usbOutputBurstPackets)
            .apply()
        engine.configureUsbBursts(usbInputBurstPackets, usbOutputBurstPackets)
    }
    fun startMonitoring() {
        syncEngine()
        ContextCompat.startForegroundService(
            context,
            Intent(context, AudioProcessingService::class.java).setAction(AudioProcessingService.ACTION_START)
        )
    }
    Scaffold(containerColor = Ink, topBar = { TopAppBar(title = { Row(verticalAlignment = Alignment.CenterVertically) { Icon(Icons.Outlined.GraphicEq, null, tint = Teal, modifier = Modifier.size(25.dp)); Spacer(Modifier.width(9.dp)); Text("LiveAudioProcess", fontWeight = FontWeight.Bold, letterSpacing = 1.5.sp) } }, actions = { StatusDot(running) }, colors = TopAppBarDefaults.topAppBarColors(containerColor = Ink, titleContentColor = Color.White)) }) { pad ->
        Column(Modifier.fillMaxSize().padding(pad).padding(horizontal = 16.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            Spacer(Modifier.height(2.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.Bottom) { Column { Text("实时监听", color = Color.White, fontSize = 28.sp, fontWeight = FontWeight.Bold); Text("LOW-LATENCY DSP CONSOLE", color = Muted, fontSize = 11.sp, letterSpacing = 1.2.sp) }; Text(if (running) "RUNNING" else "STANDBY", color = if (running) Teal else Muted, fontSize = 12.sp, fontWeight = FontWeight.Bold) }
            ScreenAlwaysOnOption(screenAlwaysOn) { enabled ->
                screenAlwaysOn = enabled
                prefs.edit().putBoolean("screenAlwaysOn", enabled).apply()
            }
            LevelPanel(inputLevelL, inputLevelR, outputLevelL, outputLevelR, limiterGain, limiterReleaseMs, running, running && effects.dspEnabled && effects.limiterEnabled)
            RoutingPanel2(input, { selected -> input = selected; channelPair = 0; if (selected == InputSource.WIFI) wifiOutputEnabled = false; wifiActive = configureWifiForCurrentRoute(); syncEngine() }, output, { selected -> if (selected == OutputSource.BLUETOOTH && Build.VERSION.SDK_INT >= 31 && ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) { pendingBluetoothOutput = selected; bluetoothPermissionLauncher.launch(Manifest.permission.BLUETOOTH_CONNECT) } else { output = selected; syncEngine() } }, wifiOutputEnabled, { enabled -> if (input != InputSource.WIFI) { wifiOutputEnabled = enabled; wifiActive = configureWifiForCurrentRoute(); syncEngine() } }, channelPairs, channelPair, { channelPair = it; syncEngine() }, routeNotice)
            EnginePanel(rate, { rate = it; syncEngine() }, buffer, { buffer = it; syncEngine() }, running)
            if (input == InputSource.USB || output == OutputSource.USB) {
                UsbAudioPanel(usbMinBuffer, usbMaxBuffer, usbBitDepth, usbInputBurstPackets, usbOutputBurstPackets, input == InputSource.USB, output == OutputSource.USB, running, usbStats, { usbMinBuffer = it }, { usbMaxBuffer = it }, { usbBitDepth = it }, { usbInputBurstPackets = it }, { usbOutputBurstPackets = it })
            }
            Float32Badge()
            ProcessingControlPanel(effects) { effects = it }
            UnifiedEffectsPanel(effects) { effects = it }
            EqBandsPanel(effects) { effects = it }
            WifiAudioPanel(wifiSendHost, wifiSendPort, wifiOutputEnabled && wifiActive, { wifiSendHost = it }, { wifiSendPort = it }, wifiReceiveHost, wifiReceivePort, wifiMinBuffer, wifiMaxBuffer, wifiInputTimeout, input == InputSource.WIFI && wifiActive, { wifiReceiveHost = it }, { wifiReceivePort = it }, { wifiMinBuffer = it }, { wifiMaxBuffer = it }, { wifiInputTimeout = it })
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                Button(onClick = {
                    if (!hasPermission) {
                        permissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
                    } else if (running) {
                        context.startService(Intent(context, AudioProcessingService::class.java).setAction(AudioProcessingService.ACTION_STOP))
                    } else if (input == InputSource.USB || output == OutputSource.USB) {
                        val currentActivity = activity as? MainActivity
                        if (currentActivity == null) {
                            startMonitoring()
                        } else {
                            currentActivity.requestUsbAudioPermission { granted ->
                                if (granted) startMonitoring()
                                else routeNotice = "USB AUDIO CODEC 权限被拒绝，未启动监听"
                            }
                        }
                    } else {
                        startMonitoring()
                    }
                }, modifier = Modifier.weight(1f).height(54.dp), colors = ButtonDefaults.buttonColors(containerColor = if (running) PanelRaised else Teal, contentColor = if (running) Color.White else Ink), shape = RoundedCornerShape(10.dp)) { Icon(if (running) Icons.Outlined.Stop else Icons.Outlined.PlayArrow, null); Spacer(Modifier.width(8.dp)); Text(if (!hasPermission) "授权麦克风" else if (running) "停止引擎" else "启动监听", fontWeight = FontWeight.Bold) }
                IconButton(onClick = { if (running) { recording = !recording; engine.setRecording(recording); if (!recording) elapsed = 0 } }, modifier = Modifier.size(54.dp).background(if (recording) Red.copy(.18f) else Panel, RoundedCornerShape(10.dp))) { Icon(Icons.Outlined.FiberManualRecord, "录音", tint = if (recording) Red else Muted) }
            }
            RecordingBar(recording, elapsed)
            Spacer(Modifier.height(12.dp))
        }
    }
}

@Composable private fun StatusDot(active: Boolean) { Row(Modifier.padding(end = 16.dp), verticalAlignment = Alignment.CenterVertically) { Box(Modifier.size(8.dp).clip(RoundedCornerShape(50)).background(if (active) Teal else Muted)); Spacer(Modifier.width(6.dp)); Text(if (active) "LIVE" else "OFFLINE", color = if (active) Teal else Muted, fontSize = 11.sp, fontWeight = FontWeight.Bold) } }
@Composable private fun ScreenAlwaysOnOption(enabled: Boolean, onEnabledChange: (Boolean) -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = Panel),
        shape = RoundedCornerShape(12.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text("屏幕常亮", color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                Text("保持屏幕唤醒，避免监视过程中自动息屏", color = Muted, fontSize = 11.sp)
            }
            Switch(checked = enabled, onCheckedChange = onEnabledChange)
        }
    }
}
@Composable private fun LevelPanel(inputL: Float, inputR: Float, outputL: Float, outputR: Float, limiterGain: Float, limiterReleaseMs: Float, active: Boolean, limiterActive: Boolean) { Card(colors = CardDefaults.cardColors(containerColor = Panel), shape = RoundedCornerShape(12.dp), modifier = Modifier.fillMaxWidth()) { Column(Modifier.padding(16.dp)) { Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text("信号电平", color = Color.White, fontWeight = FontWeight.SemiBold); Text("峰值监视 · ${if (active) "实时" else "待机"}", color = Muted, fontSize = 12.sp) }; Spacer(Modifier.height(13.dp)); MeterRow("INPUT L / DRY", inputL, Teal); Spacer(Modifier.height(6.dp)); MeterRow("INPUT R / DRY", inputR, Teal); Spacer(Modifier.height(8.dp)); MeterRow("OUTPUT L / WET", outputL, Amber); Spacer(Modifier.height(6.dp)); MeterRow("OUTPUT R / WET", outputR, Amber); Spacer(Modifier.height(12.dp)); HorizontalDivider(color = Color(0xFF344248)); Spacer(Modifier.height(9.dp)); Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text("LIMITER", color = Color.White, fontSize = 11.sp, fontWeight = FontWeight.Bold); Text(if (limiterActive) "ACTIVE" else "BYPASS", color = if (limiterActive) Teal else Muted, fontSize = 10.sp, fontWeight = FontWeight.Bold) }; Spacer(Modifier.height(5.dp)); Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text("GAIN", color = Muted, fontSize = 10.sp); Text("%.6f".format(limiterGain), color = Color.White, fontSize = 11.sp); Text("RELEASE", color = Muted, fontSize = 10.sp); Text(limiterReleaseStatus(limiterReleaseMs), color = Color.White, fontSize = 11.sp) } } } }
@Composable private fun MeterRow(label: String, level: Float, tint: Color) { Row(verticalAlignment = Alignment.CenterVertically) { Text(label, color = Muted, fontSize = 10.sp, modifier = Modifier.width(86.dp)); LinearProgressIndicator(progress = { level.coerceIn(0f, 1f) }, modifier = Modifier.weight(1f).height(7.dp).clip(RoundedCornerShape(4.dp)), color = tint, trackColor = Color(0xFF2C3B40)); Text("${(-60 + level * 60).toInt()} dB", color = Color.White, fontSize = 11.sp, modifier = Modifier.width(52.dp).padding(start = 8.dp)) } }
private fun limiterReleaseStatus(valueMs: Float): String = if (valueMs >= 1000f) "%.2f s".format(valueMs / 1000f) else "%.1f ms".format(valueMs)
@Composable private fun RoutingPanel(input: InputSource, onInput: (InputSource) -> Unit, output: OutputSource, onOutput: (OutputSource) -> Unit, channelPairs: List<ChannelPair>, selectedPair: Int, onPair: (Int) -> Unit) { Card(colors = CardDefaults.cardColors(containerColor = Panel), shape = RoundedCornerShape(12.dp), modifier = Modifier.fillMaxWidth()) { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) { SectionTitle("路由", "I/O ROUTING"); Text("输入源", color = Muted, fontSize = 12.sp); ChoiceRow(InputSource.values().toList(), input, onInput); if (input == InputSource.USB) { Text("输入通道对", color = Muted, fontSize = 12.sp); ChoiceRow(channelPairs, channelPairs.firstOrNull { it.index == selectedPair } ?: channelPairs.first(), { onPair(it.index) }); Text("USB 多通道会以所选立体声通道对进入 DSP，干声录音保留左右声道。", color = Muted, fontSize = 11.sp) }; Text("输出目标", color = Muted, fontSize = 12.sp); ChoiceRow(OutputSource.values().toList(), output, onOutput); if (output == OutputSource.BLUETOOTH) Text("蓝牙链路通常带来 150–200 ms 延迟，建议使用有线或 USB 输出。", color = Amber, fontSize = 11.sp) } } }
@Composable private fun <T> ChoiceRow(items: List<T>, selected: T, onSelect: (T) -> Unit) { Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) { items.forEach { item -> val label = when (item) { is InputSource -> item.label; is OutputSource -> item.label; is ChannelPair -> item.label; is Int -> if (item < 1000) "$item samples" else if (item % 1000 == 0) "${item / 1000} kHz" else "${item / 1000f} kHz"; else -> item.toString() }; FilterChip(selected = selected == item, onClick = { onSelect(item) }, label = { Text(label, fontSize = 12.sp) }, leadingIcon = { if (item is InputSource || item is OutputSource) Icon(if (item is InputSource && item == InputSource.BUILT_IN) Icons.Outlined.Mic else if (item is OutputSource && item == OutputSource.BLUETOOTH) Icons.Outlined.Bluetooth else if (item is OutputSource) Icons.Outlined.Headphones else Icons.Outlined.Usb, null, modifier = Modifier.size(16.dp)) }) } } }

@Composable private fun RoutingPanel2(input: InputSource, onInput: (InputSource) -> Unit, output: OutputSource, onOutput: (OutputSource) -> Unit, wifiOutput: Boolean, onWifiOutput: (Boolean) -> Unit, channelPairs: List<ChannelPair>, selectedPair: Int, onPair: (Int) -> Unit, routeNotice: String?) {
    Card(colors = CardDefaults.cardColors(containerColor = Panel), shape = RoundedCornerShape(12.dp), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            SectionTitle("路由", "I/O ROUTING")
            Text("输入源", color = Muted, fontSize = 12.sp)
            ChoiceRow(InputSource.values().toList(), input, onInput)
            if (input == InputSource.USB) {
                Text("输入通道对", color = Muted, fontSize = 12.sp)
                ChoiceRow(channelPairs, channelPairs.firstOrNull { it.index == selectedPair } ?: channelPairs.first()) { onPair(it.index) }
                Text("USB 多通道以所选立体声通道对进入 DSP。", color = Muted, fontSize = 11.sp)
            }
            Text("本地输出", color = Muted, fontSize = 12.sp)
            ChoiceRow(OutputSource.values().filter { it != OutputSource.WIFI }, output, onOutput)
            if (output == OutputSource.BLUETOOTH) Text("蓝牙通常带来 150–200 ms 延迟，建议使用有线或 USB。", color = Amber, fontSize = 11.sp)
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween) {
                Column(Modifier.weight(1f)) {
                    Text("Wi-Fi 输出", color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                    Text(if (input == InputSource.WIFI) "Wi-Fi 接收端启用时暂停网络发送" else if (wifiOutput) "处理后的音频同时发送到网络" else "可与本地扬声器或 USB 输出同时使用", color = Muted, fontSize = 11.sp)
                }
                Switch(checked = wifiOutput, onCheckedChange = onWifiOutput, enabled = input != InputSource.WIFI)
            }
            if (!routeNotice.isNullOrBlank()) Text(routeNotice, color = Amber, fontSize = 11.sp)
        }
    }
}
@Composable private fun EnginePanel(rate: Int, onRate: (Int) -> Unit, buffer: Int, onBuffer: (Int) -> Unit, active: Boolean) { Card(colors = CardDefaults.cardColors(containerColor = Panel), shape = RoundedCornerShape(12.dp), modifier = Modifier.fillMaxWidth()) { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) { SectionTitle("引擎", "AUDIO ENGINE"); Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text("采样率", color = Muted, fontSize = 12.sp); Text(if (rate % 1000 == 0) "${rate / 1000} kHz" else "${rate / 1000f} kHz", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold) }; ChoiceRow(listOf(44_100, 48_000, 96_000), rate, onRate); Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Text("缓冲区", color = Muted, fontSize = 12.sp); Text("$buffer samples  ·  ${(buffer * 1000f / rate).toInt()} ms", color = Color.White, fontSize = 12.sp) }; ChoiceRow(listOf(128, 256, 512), buffer, onBuffer); Text(if (active) "音频线程优先级：REALTIME  ·  目标往返延迟 ≤ 30 ms" else "启动后将优先使用低延迟音频路径", color = if (active) Teal else Muted, fontSize = 11.sp) } } }
@Composable private fun EffectsPanel(settings: EffectSettings, onChange: (EffectSettings) -> Unit) { Card(colors = CardDefaults.cardColors(containerColor = Panel), shape = RoundedCornerShape(12.dp), modifier = Modifier.fillMaxWidth()) { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) { SectionTitle("处理链", "DSP CHAIN  ·  EQ → CONVOLUTION REVERB → LIMITER"); Text("参数均衡 EQ · 4-band ready", color = Teal, fontSize = 11.sp, fontWeight = FontWeight.Bold); EffectSlider("频点 1 · 频率", "20 Hz – 20 kHz", settings.eqFrequency, 20f..20000f, { onChange(settings.copy(eqFrequency = it)) }, "${settings.eqFrequency.toInt()} Hz"); EffectSlider("频点 1 · 增益", "Peaking  ·  Q 可调", settings.eqGain, -12f..12f, { onChange(settings.copy(eqGain = it)) }, "${if (settings.eqGain >= 0) "+" else ""}${settings.eqGain.toInt()} dB"); EffectSlider("频点 1 · Q 值", "带宽控制", settings.eqQ, .5f..10f, { onChange(settings.copy(eqQ = it)) }, "Q ${"%.2f".format(settings.eqQ)}"); Text("其余 EQ 频点可在同一处理器配置中扩展，当前 DSP 使用频点 1。", color = Muted, fontSize = 10.sp); Text("卷积混响 Convolution Reverb", color = Teal, fontSize = 11.sp, fontWeight = FontWeight.Bold); EffectSlider("Room size", "空间大小", settings.reverbRoom, 0f..100f, { onChange(settings.copy(reverbRoom = it)) }, "${settings.reverbRoom.toInt()}%"); EffectSlider("Decay", "衰减时间", settings.reverbDecay, .2f..6f, { onChange(settings.copy(reverbDecay = it)) }, "${"%.1f".format(settings.reverbDecay)} s"); EffectSlider("Damping", "高频阻尼", settings.reverbDamping, 0f..100f, { onChange(settings.copy(reverbDamping = it)) }, "${settings.reverbDamping.toInt()}%"); EffectSlider("Dry / Wet", "混合比例", settings.reverbMix, 0f..100f, { onChange(settings.copy(reverbMix = it)) }, "${settings.reverbMix.toInt()}%"); Text("限制器 Limiter", color = Teal, fontSize = 11.sp, fontWeight = FontWeight.Bold); EffectSlider("Threshold", "触发阈值", settings.limiterThreshold, -24f..0f, { onChange(settings.copy(limiterThreshold = it)) }, "${"%.2f".format(settings.limiterThreshold)} dB"); EffectSlider("Release", "释放时间", settings.limiterRelease, 10f..10000f, { onChange(settings.copy(limiterRelease = it)) }, "${"%.1f".format(settings.limiterRelease)} ms"); EffectSlider("Ceiling", "输出上限", settings.limiterCeiling, -6f..0f, { onChange(settings.copy(limiterCeiling = it)) }, "${"%.2f".format(settings.limiterCeiling)} dB"); EffectSlider("Look-ahead", "前视延迟", settings.limiterLookAhead, 0f..5f, { onChange(settings.copy(limiterLookAhead = it)) }, "${"%.1f".format(settings.limiterLookAhead)} ms") } } }
@Composable private fun EffectSlider(title: String, hint: String, value: Float, range: ClosedFloatingPointRange<Float>, onChange: (Float) -> Unit, readout: String) { Column { Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) { Column { Text(title, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold); Text(hint, color = Muted, fontSize = 10.sp) }; Text(readout, color = Teal, fontSize = 12.sp, fontWeight = FontWeight.Bold) }; Slider(value = value, onValueChange = onChange, valueRange = range, modifier = Modifier.height(32.dp)) } }
@Composable private fun SectionTitle(title: String, code: String) { Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) { Text(title, color = Color.White, fontWeight = FontWeight.Bold); Text(code, color = Muted, fontSize = 10.sp, letterSpacing = .8.sp) } }
@Composable private fun RecordingBar(active: Boolean, elapsed: Int) { Row(Modifier.fillMaxWidth().border(1.dp, if (active) Red.copy(.5f) else Color(0xFF2A373C), RoundedCornerShape(10.dp)).padding(12.dp), verticalAlignment = Alignment.CenterVertically) { Box(Modifier.size(9.dp).clip(RoundedCornerShape(50)).background(if (active) Red else Muted)); Spacer(Modifier.width(9.dp)); Text(if (active) "正在录制 · 干声 + 湿声" else "双轨录音已就绪", color = Color.White, fontSize = 12.sp); Spacer(Modifier.weight(1f)); Text("%02d:%02d".format(elapsed / 60, elapsed % 60), color = if (active) Red else Muted, fontWeight = FontWeight.Bold, fontSize = 13.sp) } }
