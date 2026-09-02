package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
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

@Composable
fun ProcessingControlPanel(settings: EffectSettings, onChange: (EffectSettings) -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = Color(0xFF182126)), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("处理开关", color = Color.White, fontWeight = FontWeight.Bold)
            ToggleRow("整体 DSP", "关闭后为立体声直通", settings.dspEnabled) { onChange(settings.copy(dspEnabled = it)) }
            ToggleRow("参数均衡 EQ", "4-band", settings.eqEnabled) { onChange(settings.copy(eqEnabled = it)) }
            ToggleRow("卷积混响", "Partitioned convolution", settings.reverbEnabled) { onChange(settings.copy(reverbEnabled = it)) }
            ToggleRow("响度标准化 Loudness", "低延迟 · 目标响度 / 真峰值", settings.loudnessEnabled) { onChange(settings.copy(loudnessEnabled = it)) }
            ToggleRow("限制器 Limiter", "Input Gain / Threshold / Release", settings.limiterEnabled) { onChange(settings.copy(limiterEnabled = it)) }
        }
    }
}

@Composable private fun ToggleRow(title: String, subtitle: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) { Text(title, color = Color.White, fontSize = 13.sp); Text(subtitle, color = Color(0xFF8EA0A8), fontSize = 10.sp) }
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}
