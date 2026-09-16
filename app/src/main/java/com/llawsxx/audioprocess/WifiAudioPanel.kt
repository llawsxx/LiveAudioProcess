package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.FilterChip
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.Switch
import androidx.compose.material3.Slider
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.roundToInt

private val WifiTeal = Color(0xFF43D5C1)
private val WifiMuted = Color(0xFF8EA0A8)
internal val WifiAacBitrates = listOf(
    64_000, 96_000, 128_000, 192_000, 256_000, 320_000, 512_000, 768_000, 1_000_000
)

@Composable
fun WifiAudioPanel(
    sampleRate: Int,
    sendHost: String,
    sendPort: String,
    transport: Int,
    codec: Int,
    aacBitrate: Int,
    packetDuration: String,
    opusFrameMs: Int,
    opusProfile: Int,
    sendActive: Boolean,
    onSendHost: (String) -> Unit,
    onSendPort: (String) -> Unit,
    onTransport: (Int) -> Unit,
    onCodec: (Int) -> Unit,
    onAacBitrate: (Int) -> Unit,
    onPacketDuration: (String) -> Unit,
    onOpusFrameMs: (Int) -> Unit,
    onOpusProfile: (Int) -> Unit,
    receiveHost: String,
    receivePort: String,
    minBuffer: String,
    maxBuffer: String,
    maxHold: String,
    inputTimeout: String,
    receiveActive: Boolean,
    onReceiveHost: (String) -> Unit,
    onReceivePort: (String) -> Unit,
    onMinBuffer: (String) -> Unit,
    onMaxBuffer: (String) -> Unit,
    onMaxHold: (String) -> Unit,
    onInputTimeout: (String) -> Unit,
    clockCorrectionEnabled: Boolean,
    onClockCorrectionEnabled: (Boolean) -> Unit,
    wlanLowLatencyEnabled: Boolean,
    onWlanLowLatencyEnabled: (Boolean) -> Unit,
    dynamicBufferEnabled: Boolean,
    onDynamicBufferEnabled: (Boolean) -> Unit,
    manualBufferBias: Float,
    onManualBufferBias: (Float) -> Unit,
    clockCorrectionPpm: Int = 100,
    onClockCorrectionPpm: (Int) -> Unit = {},
    networkQosEnabled: Boolean = false,
    onNetworkQosEnabled: (Boolean) -> Unit = {},
    retransmitEnabled: Boolean = true,
    onRetransmitEnabled: (Boolean) -> Unit = {}
) {
    val requestedPacketMs = packetDuration.toIntOrNull()?.coerceIn(1, 100) ?: 20
    val opusPacketFrames = sampleRate * opusFrameMs / 1000
    val pcmFrames = (sampleRate.toLong() * requestedPacketMs / 1000L)
        .coerceAtLeast(1L)
        .let { frames ->
            if (transport == 0) frames.coerceAtMost((65_507L - 28L) / 8L)
            else frames
        }
    val aacFramesPerPacket = ((sampleRate.toLong() * requestedPacketMs + 512_000L) / 1_024_000L)
        .coerceIn(1L, 10L)
    val packetFrames = when (codec) {
        1 -> aacFramesPerPacket * 1024L
        2 -> sampleRate.toLong() * opusFrameMs / 1000L
        else -> pcmFrames
    }
    val actualPacketMs = packetFrames * 1000.0 / sampleRate
    val packetBytes = if (codec == 1) {
        28L + aacFramesPerPacket * 4L + (aacBitrate * actualPacketMs / 8000.0).toLong()
    } else if (codec == 2) {
        28L + (aacBitrate * opusFrameMs / 8000.0).toLong()
    } else {
        28L + packetFrames * 2L * 4L
    }
    val segments = ((packetBytes + if (transport == 0) 1471L else 1459L) /
        if (transport == 0) 1472L else 1460L).coerceAtLeast(1L)
    val minBufferMsValue = minBuffer.toIntOrNull()?.coerceIn(0, 200) ?: 0
    val maxBufferMsValue = maxBuffer.toIntOrNull()?.coerceIn(50, 1000) ?: 200
    val dynamicStartupTargetMs = (minBufferMsValue + (actualPacketMs * 2.0).roundToInt())
        .coerceIn(minBufferMsValue, maxBufferMsValue)
    val manualTargetMs = minBufferMsValue + ((maxBufferMsValue - minBufferMsValue) * manualBufferBias.coerceIn(0f, 1f)).roundToInt()
    val displayTargetMs = if (dynamicBufferEnabled) dynamicStartupTargetMs else manualTargetMs
    val prefillTargetFrames = sampleRate.toLong() * displayTargetMs / 1000L
    val prefillFrames = ((prefillTargetFrames + packetFrames - 1L) / packetFrames) * packetFrames
    val actualPrefillMs = prefillFrames * 1000.0 / sampleRate
    val fieldColors = OutlinedTextFieldDefaults.colors(
        focusedTextColor = Color.White,
        unfocusedTextColor = Color.White,
        focusedLabelColor = WifiTeal,
        unfocusedLabelColor = WifiMuted,
        cursorColor = WifiTeal,
        focusedBorderColor = WifiTeal,
        unfocusedBorderColor = Color(0xFF4A5A60)
    )
    Card(
        colors = CardDefaults.cardColors(containerColor = Color(0xFF182126)),
        shape = RoundedCornerShape(8.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("CLOCK DRIFT CORRECTION", color = Color.White, fontSize = 11.sp)
                    Text("Adaptive playback rate: ±1%", color = WifiMuted, fontSize = 10.sp)
                }
                Switch(checked = clockCorrectionEnabled, onCheckedChange = onClockCorrectionEnabled)
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("CORRECTION LIMIT", color = WifiMuted, fontSize = 11.sp)
                Text("±$clockCorrectionPpm ppm", color = WifiTeal, fontSize = 11.sp)
            }
            Slider(
                value = clockCorrectionPpm.coerceIn(1, 200).toFloat(),
                onValueChange = { onClockCorrectionPpm(it.roundToInt().coerceIn(1, 200)) },
                valueRange = 1f..200f,
                steps = 198,
                enabled = clockCorrectionEnabled,
                modifier = Modifier.fillMaxWidth()
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("WLAN LOW LATENCY", color = Color.White, fontSize = 11.sp)
                    Text("Android 10+ Wi-Fi performance lock", color = WifiMuted, fontSize = 10.sp)
                }
                Switch(checked = wlanLowLatencyEnabled, onCheckedChange = onWlanLowLatencyEnabled)
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("QoS / WMM PRIORITY", color = Color.White, fontSize = 11.sp)
                    Text("DSCP EF marking for lower jitter", color = WifiMuted, fontSize = 10.sp)
                }
                Switch(checked = networkQosEnabled, onCheckedChange = onNetworkQosEnabled)
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("WIFI RETRANSMIT", color = Color.White, fontSize = 11.sp)
                    Text("UDP NACK and recent-packet retry", color = WifiMuted, fontSize = 10.sp)
                }
                Switch(
                    checked = retransmitEnabled,
                    onCheckedChange = onRetransmitEnabled,
                    enabled = transport == 0
                )
            }
            Text("Wi-Fi 实时音频", color = Color.White, fontWeight = FontWeight.Bold)

            Text("传输格式", color = WifiMuted, fontSize = 12.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = transport == 0, onClick = { onTransport(0) }, label = { Text("UDP") })
                FilterChip(selected = transport == 1, onClick = { onTransport(1) }, label = { Text("TCP") })
            }
            Text("编码格式", color = WifiMuted, fontSize = 12.sp)
            Row(
                modifier = Modifier.horizontalScroll(rememberScrollState()),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                FilterChip(selected = codec == 0, onClick = { onCodec(0) }, label = { Text("PCM Float32") })
                FilterChip(selected = codec == 1, onClick = { onCodec(1) }, label = { Text("AAC-LC") })
                FilterChip(selected = codec == 2, onClick = { onCodec(2) }, label = { Text("Opus") })
            }

            if (codec == 1) {
                Text("AAC 码率", color = WifiMuted, fontSize = 12.sp)
                Row(
                    modifier = Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    WifiAacBitrates.forEach { bitrate ->
                        FilterChip(
                            selected = aacBitrate == bitrate,
                            onClick = { onAacBitrate(bitrate) },
                            label = { Text(if (bitrate == 1_000_000) "1 Mbps" else "${bitrate / 1000} kbps") }
                        )
                    }
                }
            }
            if (codec == 2) {
                Text("Opus 码率", color = WifiMuted, fontSize = 12.sp)
                Row(modifier = Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    WifiAacBitrates.filter { it <= 512_000 }.forEach { bitrate ->
                        FilterChip(selected = aacBitrate == bitrate, onClick = { onAacBitrate(bitrate) }, label = { Text("${bitrate / 1000} kbps") })
                    }
                }
                Text("Opus 帧长", color = WifiMuted, fontSize = 12.sp)
                Row(
                    modifier = Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    listOf(5, 10, 20, 40, 60).forEach { ms ->
                        FilterChip(selected = opusFrameMs == ms, onClick = { onOpusFrameMs(ms) }, label = { Text("$ms ms") })
                    }
                }
                Text("Opus Profile", color = WifiMuted, fontSize = 12.sp)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf("Audio", "VoIP", "Low-delay").forEachIndexed { index, label ->
                        FilterChip(selected = opusProfile == index, onClick = { onOpusProfile(index) }, label = { Text(label) })
                    }
                }
            }

            EndpointHeader("发送端", "音频输出", sendActive)
            Text(
                if (codec == 1) "AAC-LC · 双声道 · $aacFramesPerPacket × 1024 frames"
                else if (codec == 2) "Opus · 双声道 · $opusFrameMs ms · $opusPacketFrames frames"
                else "PCM Float32 · 双声道 · $packetFrames frames",
                color = WifiTeal,
                fontSize = 11.sp
            )
            OutlinedTextField(
                packetDuration,
                onPacketDuration,
                label = { Text("发包时长 ms") },
                singleLine = true,
                colors = fieldColors,
                modifier = Modifier.fillMaxWidth()
            )
            Text(
                buildString {
                    append("实际 %.1f ms/包 · 约 %.1f 包/秒 · 约 %,.0f B/包".format(actualPacketMs, 1000.0 / actualPacketMs, packetBytes.toDouble()))
                    if (packetBytes > if (transport == 0) 1472 else 1460) {
                        append(if (transport == 0) " · 超过 1500 MTU，约 $segments 个 IP 分片" else " · 约 $segments 个 TCP 段")
                    } else {
                        append(" · 未超过 1500 MTU")
                    }
                },
                color = if (transport == 0 && packetBytes > 1472) Color(0xFFFFB86B) else WifiMuted,
                fontSize = 10.sp
            )
            OutlinedTextField(
                sendHost,
                onSendHost,
                label = { Text("目标设备 IP") },
                singleLine = true,
                colors = fieldColors,
                modifier = Modifier.fillMaxWidth()
            )
            OutlinedTextField(
                sendPort,
                onSendPort,
                label = { Text("目标端口") },
                singleLine = true,
                colors = fieldColors,
                modifier = Modifier.fillMaxWidth()
            )

            HorizontalDivider(color = Color(0xFF344248))

            EndpointHeader("接收端", "音频输入", receiveActive)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                OutlinedTextField(
                    receiveHost,
                    onReceiveHost,
                    label = { Text("本机监听 IP") },
                    singleLine = true,
                    colors = fieldColors,
                    modifier = Modifier.weight(1f)
                )
                OutlinedTextField(
                    receivePort,
                    onReceivePort,
                    label = { Text("监听端口") },
                    singleLine = true,
                    colors = fieldColors,
                    modifier = Modifier.weight(1f)
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                OutlinedTextField(
                    minBuffer,
                    onMinBuffer,
                    label = { Text("最小缓冲 ms") },
                    singleLine = true,
                    colors = fieldColors,
                    modifier = Modifier.weight(1f)
                )
                OutlinedTextField(
                    maxBuffer,
                    onMaxBuffer,
                    label = { Text("最大缓冲 ms") },
                    singleLine = true,
                    colors = fieldColors,
                    modifier = Modifier.weight(1f)
                )
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("目标缓冲模式", color = Color.White, fontSize = 12.sp)
                    Text(if (dynamicBufferEnabled) "动态计算" else "手动设置", color = WifiMuted, fontSize = 10.sp)
                }
                Switch(checked = dynamicBufferEnabled, onCheckedChange = onDynamicBufferEnabled)
            }
            if (!dynamicBufferEnabled) {
                Text("手动目标：$manualTargetMs ms", color = WifiTeal, fontSize = 11.sp)
                Slider(
                    value = manualBufferBias.coerceIn(0f, 1f),
                    onValueChange = onManualBufferBias,
                    valueRange = 0f..1f,
                    steps = 19,
                    modifier = Modifier.fillMaxWidth()
                )
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("最小", color = WifiMuted, fontSize = 10.sp)
                    Text("最大", color = WifiMuted, fontSize = 10.sp)
                }
            }
            OutlinedTextField(maxHold, onMaxHold, label = { Text("超过最大持续 ms") }, singleLine = true, colors = fieldColors, modifier = Modifier.fillMaxWidth())
            OutlinedTextField(
                inputTimeout,
                onInputTimeout,
                label = { Text("无数据回退时间 s") },
                singleLine = true,
                colors = fieldColors,
                modifier = Modifier.fillMaxWidth()
            )
            Text("缓冲范围：最小 0–200 ms，最大 50–1000 ms", color = WifiMuted, fontSize = 10.sp)
            Text(
                "开始播放预填充约 %.1f ms（目标 %d ms）；播放中缓冲耗尽后会重新预填充".format(actualPrefillMs, displayTargetMs),
                color = WifiTeal,
                fontSize = 10.sp
            )
        }
    }
}

@Composable
private fun EndpointHeader(title: String, subtitle: String, active: Boolean) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column {
            Text(title, color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            Text(subtitle, color = WifiMuted, fontSize = 10.sp)
        }
        Text(
            if (active) "运行中" else "未启用",
            color = if (active) WifiTeal else WifiMuted,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold
        )
    }
}
