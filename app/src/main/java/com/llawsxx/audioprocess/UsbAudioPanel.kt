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
        }
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
