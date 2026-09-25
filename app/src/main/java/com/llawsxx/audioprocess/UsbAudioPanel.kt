package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun UsbAudioPanel(
    maxBuffer: String,
    inputMaxBuffer: String,
    inputBitDepth: Int,
    outputBitDepth: Int,
    outputDitherEnabled: Boolean,
    inputBurstPackets: Int,
    outputBurstPackets: Int,
    inputEnabled: Boolean,
    outputEnabled: Boolean,
    active: Boolean,
    stats: LongArray,
    onMaxBuffer: (String) -> Unit,
    onInputMaxBuffer: (String) -> Unit,
    onInputBitDepth: (Int) -> Unit,
    onOutputBitDepth: (Int) -> Unit,
    onOutputDitherEnabled: (Boolean) -> Unit,
    onInputBurstPackets: (Int) -> Unit,
    onOutputBurstPackets: (Int) -> Unit
) {
    val accent = Color(0xFFFFC857)
    val muted = Color(0xFF8EA0A8)
    val fieldColors = OutlinedTextFieldDefaults.colors(
        focusedTextColor = Color.White,
        unfocusedTextColor = Color.White,
        focusedLabelColor = accent,
        unfocusedLabelColor = muted,
        cursorColor = accent,
        focusedBorderColor = accent,
        unfocusedBorderColor = Color(0xFF4A5A60)
    )
    Card(
        colors = CardDefaults.cardColors(containerColor = Color(0xFF182126)),
        shape = RoundedCornerShape(8.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("USB Host", color = Color.White, fontWeight = FontWeight.Bold)
            Text("USB 输入 PCM 位深", color = muted, fontSize = 12.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(16, 24, 32).forEach { bits ->
                    FilterChip(
                        selected = inputBitDepth == bits,
                        onClick = { onInputBitDepth(bits) },
                        enabled = inputEnabled,
                        label = { Text("$bits-bit", fontSize = 12.sp) }
                    )
                }
            }
            Text("USB 输出 PCM 位深", color = muted, fontSize = 12.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf(16, 24, 32).forEach { bits ->
                    FilterChip(selected = outputBitDepth == bits, onClick = { onOutputBitDepth(bits) }, enabled = outputEnabled, label = { Text("$bits-bit", fontSize = 12.sp) })
                }
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Column(Modifier.weight(1f)) {
                    Text("输出 TPDF 抖动", color = Color.White, fontSize = 12.sp)
                    Text("降低 16/24-bit 量化失真", color = muted, fontSize = 10.sp)
                }
                Switch(
                    checked = outputDitherEnabled,
                    onCheckedChange = onOutputDitherEnabled,
                    enabled = outputEnabled && outputBitDepth != 32
                )
            }
            OutlinedTextField(
                value = maxBuffer,
                onValueChange = { if (it.length <= 3 && it.all(Char::isDigit)) onMaxBuffer(it) },
                label = { Text("输出缓冲上限 ms") },
                singleLine = true,
                enabled = outputEnabled,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                colors = fieldColors,
                modifier = Modifier.fillMaxWidth()
            )
            Text("范围：5–200 ms", color = muted, fontSize = 10.sp)
            OutlinedTextField(
                value = inputMaxBuffer,
                onValueChange = { if (it.length <= 3 && it.all(Char::isDigit)) onInputMaxBuffer(it) },
                label = { Text("输入缓冲上限 ms") },
                singleLine = true,
                enabled = inputEnabled,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                colors = fieldColors,
                modifier = Modifier.fillMaxWidth()
            )
            Text("输入范围：5–200 ms，超过上限自动清空旧数据", color = muted, fontSize = 10.sp)
            BurstChoice("输入 Burst（USB 包 / transfer）", inputBurstPackets, inputEnabled, muted, onInputBurstPackets)
            BurstChoice("输出 Burst（USB 包 / transfer）", outputBurstPackets, outputEnabled, muted, onOutputBurstPackets)
            HorizontalDivider(color = Color(0xFF344248))
            Text("实际采用格式", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            if (inputEnabled) {
                UsbFormatRow("输入", active, stats.getOrElse(6) { 0 }, stats.getOrElse(7) { 0 }, stats.getOrElse(8) { 0 }, muted, accent)
            }
            if (outputEnabled) {
                UsbFormatRow("输出", active, stats.getOrElse(9) { 0 }, stats.getOrElse(10) { 0 }, stats.getOrElse(11) { 0 }, muted, accent)
            }
            HorizontalDivider(color = Color(0xFF344248))
            Text("本次 USB 异常", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            UsbStatPair("输入异常包", stats.getOrElse(0) { 0 }, "输入空包", stats.getOrElse(1) { 0 }, muted)
            UsbStatPair("输入传输异常", stats.getOrElse(2) { 0 }, "输入 Ring 溢出", stats.getOrElse(3) { 0 }, muted)
            UsbStatPair("输出传输异常", stats.getOrElse(4) { 0 }, "输出低水位", stats.getOrElse(5) { 0 }, muted)
            UsbStatPair("输入回调峰值", stats.getOrElse(12) { 0 }, "输出回调峰值", stats.getOrElse(13) { 0 }, muted, " μs", false)
            UsbStatPair("输出 Ring 溢出", stats.getOrElse(14) { 0 }, "输入缓冲清空", stats.getOrElse(15) { 0 }, muted)
            UsbStatPair("DSP 本块 (μs)", stats.getOrElse(16) { 0 }, "DSP 峰值 (μs)", stats.getOrElse(17) { 0 }, muted, "", false)
        }
    }
}

@Composable
private fun UsbFormatRow(
    label: String,
    active: Boolean,
    sampleRate: Long,
    bitDepth: Long,
    channels: Long,
    muted: Color,
    accent: Color
) {
    val actualFormat = when {
        !active -> "未启动"
        sampleRate <= 0 || bitDepth <= 0 || channels <= 0 -> "未采用 USB Host"
        else -> {
            val rate = if (sampleRate % 1_000L == 0L) "${sampleRate / 1_000L} kHz" else "${sampleRate / 1_000f} kHz"
            "$rate · $bitDepth-bit · $channels ch"
        }
    }
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
        Text("${label}实际格式", color = muted, fontSize = 11.sp)
        Text(
            actualFormat,
            color = if (sampleRate > 0) accent else muted,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold
        )
    }
}

@Composable
private fun UsbStatPair(
    firstLabel: String,
    firstValue: Long,
    secondLabel: String,
    secondValue: Long,
    muted: Color,
    suffix: String = "",
    alertNonzero: Boolean = true
) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        UsbStat(firstLabel, firstValue, muted, Modifier.weight(1f), suffix, alertNonzero)
        UsbStat(secondLabel, secondValue, muted, Modifier.weight(1f), suffix, alertNonzero)
    }
}

@Composable
private fun UsbStat(label: String, value: Long, muted: Color, modifier: Modifier = Modifier, suffix: String = "", alertNonzero: Boolean = true) {
    Row(modifier, horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, color = muted, fontSize = 11.sp)
        Text(
            value.toString() + suffix,
            color = if (!alertNonzero) muted else if (value == 0L) Color(0xFF43D5C1) else Color(0xFFFF6B6B),
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold
        )
    }
}

@Composable
private fun BurstChoice(
    label: String,
    selectedPackets: Int,
    enabled: Boolean,
    muted: Color,
    onSelected: (Int) -> Unit
) {
    Text(label, color = muted, fontSize = 12.sp)
    Row(
        modifier = Modifier.horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        listOf(1, 2, 4, 8, 16, 24, 32, 48 ,64, 128).forEach { packets ->
            FilterChip(
                selected = selectedPackets == packets,
                onClick = { onSelected(packets) },
                enabled = enabled,
                label = { Text("$packets 包", fontSize = 12.sp) }
            )
        }
    }
}
