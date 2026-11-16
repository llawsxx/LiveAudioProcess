package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.Locale

@Composable
fun InputClockPanel(info: LongArray, enabled: Boolean, onEnabledChange: (Boolean) -> Unit, running: Boolean, hasOutput: Boolean) {
    val stats = InputClockStats.fromNative(info)
    val muted = Color(0xFF8EA0A8)
    val teal = Color(0xFF43D5C1)
    val outputAvailable = hasOutput && stats.status != InputClockStatus.NO_OUTPUT
    val estimatedRate = stats.estimatedRateHz?.takeIf { running && outputAvailable }
    val active = running && outputAvailable && enabled && stats.status == InputClockStatus.ACTIVE
    val status = when {
        !running -> "启动后开始统计输入、输出设备时钟"
        !outputAvailable -> "无本地音频输出设备，忽略补偿"
        !enabled -> "补偿已关闭，仍统计输入采样率"
        stats.status == InputClockStatus.ACTIVE -> "正在以输出设备时钟为基准补偿输入"
        stats.status == InputClockStatus.NO_INPUT -> "当前没有可统计的音频输入设备"
        stats.status == InputClockStatus.WAITING -> "等待有效的输入、输出时钟，暂不补偿"
        else -> "正在统计采样率，初次测量至少需要 5 秒"
    }
    Card(
        colors = CardDefaults.cardColors(containerColor = Color(0xFF182126)),
        shape = RoundedCornerShape(8.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Text("输入设备时钟校正", color = Color.White, fontWeight = FontWeight.Bold)
                Switch(checked = enabled, onCheckedChange = onEnabledChange)
            }
            Text(status, color = if (active) teal else muted, fontSize = 11.sp)
            Text(
                "统计输入采样率：${estimatedRate?.let { String.format(Locale.US, "%.3f Hz", it) } ?: "— Hz"}",
                color = if (estimatedRate != null) teal else Color.White,
                fontSize = 16.sp,
                fontWeight = FontWeight.SemiBold
            )
            if (running && stats.inputRate > 0) {
                Text("输入标称：${stats.inputRate} Hz · 输出基准：${if (outputAvailable && stats.outputRate > 0) "${stats.outputRate} Hz" else "无"}", color = muted, fontSize = 11.sp)
            }
            if (estimatedRate != null) {
                val appliedPpm = if (active) stats.appliedPpm else 0.0
                Text(
                    String.format(Locale.US, "输入漂移：%+.2f ppm · 实际补偿：%+.2f ppm", stats.driftPpm, appliedPpm),
                    color = Color.White,
                    fontSize = 11.sp
                )
                Text(String.format(Locale.US, "统计窗口：%.1f 秒", stats.windowSeconds), color = muted, fontSize = 10.sp)
            }
            if (running && outputAvailable && stats.inputClockSource > 0 && stats.outputClockSource > 0) {
                Text("输入：${clockSourceLabel(stats.inputClockSource)} · 输出：${clockSourceLabel(stats.outputClockSource)}", color = muted, fontSize = 10.sp)
            }
            Text("采样率相对于输出时钟估算，并非绝对硬件频率；通过平滑重采样补偿，限幅 ±5000 ppm", color = muted, fontSize = 10.sp)
            if (running && (stats.inputClockSource > 1 || stats.outputClockSource > 1)) {
                Text("回调或 USB 传输统计包含调度抖动，估算值会逐步稳定", color = muted, fontSize = 10.sp)
            }
        }
    }
}

private fun clockSourceLabel(source: Int): String = when (source) {
    1 -> "AAudio 时间戳"
    2 -> "AAudio 回调计数"
    3 -> "USB 传输计数"
    else -> "等待时钟"
}
