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
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

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
    sendActive: Boolean,
    onSendHost: (String) -> Unit,
    onSendPort: (String) -> Unit,
    onTransport: (Int) -> Unit,
    onCodec: (Int) -> Unit,
    onAacBitrate: (Int) -> Unit,
    onPacketDuration: (String) -> Unit,
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
    onInputTimeout: (String) -> Unit
) {
    val requestedPacketMs = packetDuration.toIntOrNull()?.coerceIn(1, 100) ?: 20
    val pcmFrames = ((sampleRate.toLong() * requestedPacketMs / 1000L) / 128L * 128L)
        .coerceAtLeast(128L)
        .let { frames ->
            if (transport == 0) frames.coerceAtMost(((65_507L - 28L) / 8L / 128L) * 128L)
            else frames
        }
    val aacFramesPerPacket = ((sampleRate.toLong() * requestedPacketMs + 512_000L) / 1_024_000L)
        .coerceIn(1L, 10L)
    val packetFrames = if (codec == 1) aacFramesPerPacket * 1024L else pcmFrames
    val actualPacketMs = packetFrames * 1000.0 / sampleRate
    val packetBytes = if (codec == 1) {
        28L + aacFramesPerPacket * 4L + (aacBitrate * actualPacketMs / 8000.0).toLong()
    } else {
        28L + packetFrames * 2L * 4L
    }
    val segments = ((packetBytes + if (transport == 0) 1471L else 1459L) /
        if (transport == 0) 1472L else 1460L).coerceAtLeast(1L)
    val prefillTargetMs = ((minBuffer.toIntOrNull()?.coerceIn(0, 200) ?: 0) +
        (maxBuffer.toIntOrNull()?.coerceIn(50, 1000) ?: 200)) / 2
    val prefillTargetFrames = sampleRate.toLong() * prefillTargetMs / 1000L
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
            Text("Wi-Fi 实时音频", color = Color.White, fontWeight = FontWeight.Bold)

            Text("传输格式", color = WifiMuted, fontSize = 12.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = transport == 0, onClick = { onTransport(0) }, label = { Text("UDP") })
                FilterChip(selected = transport == 1, onClick = { onTransport(1) }, label = { Text("TCP") })
            }
            Text("编码格式", color = WifiMuted, fontSize = 12.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = codec == 0, onClick = { onCodec(0) }, label = { Text("PCM Float32") })
                FilterChip(selected = codec == 1, onClick = { onCodec(1) }, label = { Text("AAC-LC") })
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

            EndpointHeader("发送端", "音频输出", sendActive)
            Text(
                if (codec == 1) "AAC-LC · 双声道 · $aacFramesPerPacket × 1024 frames"
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
                "开始播放预填充约 %.1f ms（目标 %d ms）；播放中缓冲耗尽后会重新预填充".format(actualPrefillMs, prefillTargetMs),
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
