package com.llawsxx.audioprocess

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun EnginePanelWithIoRates(inputRate: Int, onInputRate: (Int) -> Unit, outputRate: Int, onOutputRate: (Int) -> Unit, buffer: Int, onBuffer: (Int) -> Unit, active: Boolean) {
    val panel = Color(0xFF202B30)
    val muted = Color(0xFF8EA0A8)
    val teal = Color(0xFF43D5C1)
    val rates = listOf(44_100, 48_000, 96_000)
    Card(colors = CardDefaults.cardColors(containerColor = panel), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("音频引擎", color = Color.White, fontWeight = FontWeight.Bold)
            RateRow("输入采样率", inputRate, rates, onInputRate, muted)
            RateRow("输出采样率", outputRate, rates, onOutputRate, muted)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("缓冲区", color = muted, fontSize = 12.sp)
                Text("$buffer samples · ${(buffer * 1000f / inputRate).toInt()} ms", color = Color.White, fontSize = 12.sp)
            }
            Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(128, 256, 512).forEach { frames ->
                    FilterChip(selected = frames == buffer, onClick = { onBuffer(frames) }, label = { Text("$frames samples", fontSize = 12.sp) })
                }
            }
            Text(if (active) "AAudio 与 USB Host 使用上述输入/输出采样率，输出不一致时自动转换" else "启动后 AAudio 与 USB Host 将使用上述输入/输出采样率", color = if (active) teal else muted, fontSize = 11.sp)
        }
    }
}

@Composable
private fun RateRow(label: String, selectedRate: Int, rates: List<Int>, onSelected: (Int) -> Unit, muted: Color) {
    Text(label, color = muted, fontSize = 12.sp)
    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        rates.forEach { rate ->
            FilterChip(selected = rate == selectedRate, onClick = { onSelected(rate) }, label = { Text(if (rate == 44_100) "44.1 kHz" else "${rate / 1000} kHz", fontSize = 12.sp) })
        }
    }
}
