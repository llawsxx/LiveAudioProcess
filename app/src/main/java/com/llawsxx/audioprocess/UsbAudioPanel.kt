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
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun UsbAudioPanel(
    minBuffer: String,
    maxBuffer: String,
    inputBurstPackets: Int,
    outputBurstPackets: Int,
    inputEnabled: Boolean,
    outputEnabled: Boolean,
    stats: LongArray,
    onMinBuffer: (String) -> Unit,
    onMaxBuffer: (String) -> Unit,
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
            Text("输出缓冲", color = muted, fontSize = 12.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                OutlinedTextField(
                    value = minBuffer,
                    onValueChange = { if (it.all(Char::isDigit)) onMinBuffer(it) },
                    label = { Text("最小缓冲 ms") },
                    singleLine = true,
                    enabled = outputEnabled,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    colors = fieldColors,
                    modifier = Modifier.weight(1f)
                )
                OutlinedTextField(
                    value = maxBuffer,
                    onValueChange = { if (it.all(Char::isDigit)) onMaxBuffer(it) },
                    label = { Text("最大缓冲 ms") },
                    singleLine = true,
                    enabled = outputEnabled,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    colors = fieldColors,
                    modifier = Modifier.weight(1f)
                )
            }
            Text("范围：最小 8–200 ms，最大 8–500 ms", color = muted, fontSize = 10.sp)
            BurstChoice("输入 Burst（USB 包 / transfer）", inputBurstPackets, inputEnabled, muted, onInputBurstPackets)
            BurstChoice("输出 Burst（USB 包 / transfer）", outputBurstPackets, outputEnabled, muted, onOutputBurstPackets)
            HorizontalDivider(color = Color(0xFF344248))
            Text("本次 USB 状态", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            UsbStatPair("输入异常包", stats.getOrElse(0) { 0 }, "输入空包", stats.getOrElse(1) { 0 }, muted)
            UsbStatPair("输入传输异常", stats.getOrElse(2) { 0 }, "输入 Ring 溢出", stats.getOrElse(3) { 0 }, muted)
            UsbStatPair("输出传输异常", stats.getOrElse(4) { 0 }, "输出低水位", stats.getOrElse(5) { 0 }, muted)
        }
    }
}

@Composable
private fun UsbStatPair(
    firstLabel: String,
    firstValue: Long,
    secondLabel: String,
    secondValue: Long,
    muted: Color
) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        UsbStat(firstLabel, firstValue, muted, Modifier.weight(1f))
        UsbStat(secondLabel, secondValue, muted, Modifier.weight(1f))
    }
}

@Composable
private fun UsbStat(label: String, value: Long, muted: Color, modifier: Modifier = Modifier) {
    Row(modifier, horizontalArrangement = Arrangement.SpaceBetween) {
        Text(label, color = muted, fontSize = 11.sp)
        Text(
            value.toString(),
            color = if (value == 0L) Color(0xFF43D5C1) else Color(0xFFFF6B6B),
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
        listOf(1, 2, 4, 8, 16).forEach { packets ->
            FilterChip(
                selected = selectedPackets == packets,
                onClick = { onSelected(packets) },
                enabled = enabled,
                label = { Text("$packets 包", fontSize = 12.sp) }
            )
        }
    }
}
