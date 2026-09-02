package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private val OutputPanel = Color(0xFF182126)
private val OutputMuted = Color(0xFF8EA0A8)
private val OutputTeal = Color(0xFF43D5C1)
private val OutputAmber = Color(0xFFFFC857)

@Composable
fun SystemOutputPanel(info: LongArray, visible: Boolean, maxBufferMs: String, onMaxBufferMsChange: (String) -> Unit) {
    if (!visible) return
    val rate = info.getOrElse(0) { 0L }
    val active = rate > 0L
    val lowLatency = info.getOrElse(1) { 0L } == 12L
    val exclusive = info.getOrElse(2) { 1L } == 0L
    val burst = info.getOrElse(3) { 0L }
    val bufferSize = info.getOrElse(4) { 0L }
    val capacity = info.getOrElse(5) { 0L }
    val appQueued = info.getOrElse(8) { 0L }
    val systemQueued = info.getOrElse(9) { 0L }
    val xruns = info.getOrElse(7) { 0L }
    val underflows = info.getOrElse(10) { 0L }
    val latencyMs = info.getOrElse(11) { 0L } / 1000f
    val error = info.getOrElse(12) { 0L }
    val clears = info.getOrElse(13) { 0L }
    val dspLastUs = info.getOrElse(14) { 0L }
    val dspMaxUs = info.getOrElse(15) { 0L }
    val healthy = lowLatency && error == 0L

    Card(
        colors = CardDefaults.cardColors(containerColor = OutputPanel),
        shape = RoundedCornerShape(8.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("系统输出", color = Color.White, fontWeight = FontWeight.Bold)
                Text("AAUDIO CALLBACK", color = OutputMuted, fontSize = 10.sp)
            }
            Text("输出缓冲上限", color = OutputMuted, fontSize = 12.sp)
            OutlinedTextField(
                value = maxBufferMs,
                onValueChange = { value ->
                    if (value.length <= 3 && value.all(Char::isDigit)) onMaxBufferMsChange(value)
                },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                suffix = { Text("ms") },
                supportingText = { Text("5–200 ms") }
            )
            if (!active) {
                Text("启动后显示实际输出状态", color = OutputMuted, fontSize = 11.sp)
                return@Column
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("实际路径", color = OutputMuted, fontSize = 12.sp)
                Text(
                    "${if (lowLatency) "低延迟" else "普通路径"} · ${if (exclusive) "独占" else "共享"}",
                    color = if (healthy) OutputTeal else OutputAmber,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.SemiBold
                )
            }
            Text(
                "${rate / 1000f} kHz · Burst $burst 帧 · AAudio 缓冲 $bufferSize/$capacity 帧",
                color = Color.White,
                fontSize = 11.sp
            )
            Text(
                "应用队列 $appQueued 帧 + 系统队列 $systemQueued 帧 · 估算 ${"%.1f".format(latencyMs)} ms",
                color = if (latencyMs <= 40f) OutputTeal else OutputAmber,
                fontSize = 11.sp
            )
            Text(
                "DSP 本块 ${dspLastUs} μs · 峰值 ${dspMaxUs} μs",
                color = if (dspMaxUs <= 5000L || dspMaxUs == 0L) OutputMuted else OutputAmber,
                fontSize = 10.sp
            )
            Text(
                "XRUN $xruns · 回调欠载 $underflows · 缓冲清空 $clears${if (error != 0L) " · 错误 $error" else ""}",
                color = if (xruns == 0L && underflows == 0L && clears == 0L && error == 0L) OutputMuted else OutputAmber,
                fontSize = 10.sp
            )
        }
    }
}
