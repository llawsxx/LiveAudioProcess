package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private val UnifiedPanel = Color(0xFF182126)
private val UnifiedMuted = Color(0xFF8EA0A8)
private val UnifiedTeal = Color(0xFF43D5C1)

@Composable
fun UnifiedEffectsPanel(settings: EffectSettings, onChange: (EffectSettings) -> Unit) {
    Card(
        colors = CardDefaults.cardColors(containerColor = UnifiedPanel),
        shape = RoundedCornerShape(8.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(7.dp)) {
            Text("处理链参数", color = Color.White, fontWeight = FontWeight.Bold)

            GroupTitle("参数均衡 EQ · 频点 1")
            ParameterSlider("频率", settings.eqFrequency, 20f..20000f, { onChange(settings.copy(eqFrequency = it)) }, "${settings.eqFrequency.toInt()} Hz")
            ParameterSlider("增益", settings.eqGain, -12f..12f, { onChange(settings.copy(eqGain = it)) }, "${settings.eqGain.toInt()} dB")
            ParameterSlider("Q 值", settings.eqQ, .5f..10f, { onChange(settings.copy(eqQ = it)) }, "${"%.2f".format(settings.eqQ)}")

            GroupTitle("卷积混响 Convolution Reverb")
            ParameterSlider("Room size", settings.reverbRoom, 0f..100f, { onChange(settings.copy(reverbRoom = it)) }, "${settings.reverbRoom.toInt()}%")
            ParameterSlider("Decay", settings.reverbDecay, .2f..6f, { onChange(settings.copy(reverbDecay = it)) }, "${"%.1f".format(settings.reverbDecay)} s")
            ParameterSlider("Damping", settings.reverbDamping, 0f..100f, { onChange(settings.copy(reverbDamping = it)) }, "${settings.reverbDamping.toInt()}%")
            ParameterSlider("Dry / Wet", settings.reverbMix, 0f..100f, { onChange(settings.copy(reverbMix = it)) }, "${settings.reverbMix.toInt()}%")

            GroupTitle("响度标准化 Loudness · Limiter 前级")
            ParameterSlider("目标响度", settings.loudnessTarget, -30f..-5f, { onChange(settings.copy(loudnessTarget = it)) }, "${"%.1f".format(settings.loudnessTarget)} LUFS")
            ParameterSlider("响度范围 LRA", settings.loudnessLra, 1f..20f, { onChange(settings.copy(loudnessLra = it)) }, "${"%.1f".format(settings.loudnessLra)} LU")
            ParameterSlider("真峰值上限", settings.loudnessTruePeak, -9f..0f, { onChange(settings.copy(loudnessTruePeak = it)) }, "${"%.1f".format(settings.loudnessTruePeak)} dBTP")

            GroupTitle("限制器 Limiter")
            ParameterSlider("Input Gain", settings.limiterInputGain, -24f..24f, { onChange(settings.copy(limiterInputGain = it)) }, limiterDbLabel(settings.limiterInputGain))
            ParameterSlider("Threshold", settings.limiterThreshold, -24f..0f, { onChange(settings.copy(limiterThreshold = it)) }, limiterDbLabel(settings.limiterThreshold))
            ParameterSlider("Release", settings.limiterRelease, 10f..10000f, { onChange(settings.copy(limiterRelease = it)) }, releaseLabel(settings.limiterRelease))
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("自适应释放", color = Color.White, fontSize = 11.sp)
                    Text("短峰值快速恢复，持续峰值平滑恢复", color = UnifiedMuted, fontSize = 10.sp)
                }
                Switch(
                    checked = settings.limiterAdaptiveRelease,
                    onCheckedChange = { onChange(settings.copy(limiterAdaptiveRelease = it)) }
                )
            }
            ParameterSlider("Ceiling", settings.limiterCeiling, -6f..0f, { onChange(settings.copy(limiterCeiling = it)) }, limiterDbLabel(settings.limiterCeiling))
            ParameterSlider("Look-ahead", settings.limiterLookAhead, 0f..5f, { onChange(settings.copy(limiterLookAhead = it)) }, "${"%.1f".format(settings.limiterLookAhead)} ms")

        }
    }
}

@Composable
private fun GroupTitle(text: String) {
    Text(text, color = UnifiedTeal, fontSize = 11.sp, fontWeight = FontWeight.Bold, modifier = Modifier.padding(top = 5.dp))
}

@Composable
private fun ParameterSlider(label: String, value: Float, range: ClosedFloatingPointRange<Float>, onChange: (Float) -> Unit, readout: String) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Color.White, fontSize = 11.sp, modifier = Modifier.width(72.dp), maxLines = 1)
        Slider(value = value, onValueChange = onChange, valueRange = range, modifier = Modifier.weight(1f).height(30.dp))
        Spacer(Modifier.width(14.dp))
        Text(readout, color = UnifiedMuted, fontSize = 10.sp, modifier = Modifier.width(72.dp), maxLines = 1)
    }
}

private fun releaseLabel(value: Float): String =
    if (value >= 1000f) "${"%.2f".format(value / 1000f)} s" else "${value.toInt()} ms"

private fun limiterDbLabel(value: Float): String = "%.2f dB".format(value)
