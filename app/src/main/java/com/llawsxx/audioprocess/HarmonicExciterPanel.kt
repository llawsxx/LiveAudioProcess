package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.ln
import kotlin.math.pow

private val ExciterAccent = Color(0xFF43D5C1)
private val ExciterMuted = Color(0xFF8EA0A8)

@Composable
fun HarmonicExciterPanel(settings: EffectSettings, sampleRate: Int, onChange: (EffectSettings) -> Unit) {
    val exciter = settings.exciter.normalized()
    fun update(value: HarmonicExciterSettings) = onChange(settings.copy(exciter = value.normalized()))
    Card(colors = CardDefaults.cardColors(containerColor = Color(0xFF182126)), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("谐波激励器", color = Color.White, fontWeight = FontWeight.Bold)
                    Text("HARMONIC EXCITER · 4× OVERSAMPLING", color = ExciterMuted, fontSize = 10.sp)
                }
                Switch(checked = settings.exciterEnabled, onCheckedChange = { onChange(settings.copy(exciterEnabled = it)) })
            }
            Text(
                if (!settings.dspEnabled) "整体 DSP 未开启，当前仅编辑参数。" else if (!settings.exciterEnabled) "已旁路，开启后在 EQ 之后添加谐波。" else "已启用 · EQ → 谐波激励 → 混响 → 响度 → 限制器",
                color = ExciterMuted, fontSize = 11.sp
            )
            ExciterSlider("处理起点", exciter.frequency, 20f..12000f, frequencyLabel(exciter.frequency), logarithmic = true) {
                update(exciter.copy(frequency = it))
            }
            ExciterSlider("驱动", exciter.drive, 0f..24f, "%.1f dB".format(exciter.drive)) { update(exciter.copy(drive = it)) }
            ExciterSlider("谐波添加量", exciter.amount, 0f..100f, "%.0f%%".format(exciter.amount)) { update(exciter.copy(amount = it)) }
            ExciterSlider("高频上限", exciter.tone, maxOf(500f, exciter.frequency)..20000f, frequencyLabel(exciter.tone), logarithmic = true) {
                update(exciter.copy(tone = it))
            }
            ExciterSlider("偶次比例", exciter.even, 0f..100f, "偶 %.0f%% / 奇 %.0f%%".format(exciter.even, 100f - exciter.even)) {
                update(exciter.copy(even = it))
            }
            ExciterSlider("输出增益", exciter.outputGain, -12f..6f, "%+.1f dB".format(exciter.outputGain)) { update(exciter.copy(outputGain = it)) }
            Text(
                "非线性支路生成新的谐波，不只是高频 EQ。处理起点是高通截止频率，并非硬切边界；偶次比例控制两种支路的混合权重，不代表实际频谱占比。",
                color = ExciterMuted, fontSize = 10.sp
            )
            Text(
                "实际截止：${frequencyLabel(exciter.effectiveFrequency(sampleRate))} – ${frequencyLabel(exciter.effectiveTone(sampleRate))}。抗混叠滤波还会衰减接近奈奎斯特的高频。添加量先从 5–15% 试听，注意齿音与输出余量。",
                color = ExciterMuted, fontSize = 10.sp
            )
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                TextButton(onClick = { update(HarmonicExciterSettings()) }) { Text("复位参数", color = ExciterAccent) }
            }
        }
    }
}

@Composable
private fun ExciterSlider(
    title: String, value: Float, range: ClosedFloatingPointRange<Float>, readout: String,
    logarithmic: Boolean = false, onChange: (Float) -> Unit
) {
    val position = if (logarithmic) (ln(value / range.start) / ln(range.endInclusive / range.start)) else
        (value - range.start) / (range.endInclusive - range.start)
    Column {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(title, color = Color.White, fontSize = 12.sp)
            Text(readout, color = ExciterAccent, fontSize = 12.sp)
        }
        Slider(
            value = position.coerceIn(0f, 1f),
            onValueChange = {
                onChange(if (logarithmic) range.start * (range.endInclusive / range.start).toDouble().pow(it.toDouble()).toFloat()
                else range.start + it * (range.endInclusive - range.start))
            },
            modifier = Modifier.fillMaxWidth().height(32.dp).semantics { contentDescription = "谐波激励 $title" }
        )
    }
}

private fun frequencyLabel(frequency: Float): String = if (frequency >= 1000f) "%.2f kHz".format(frequency / 1000f) else "%.0f Hz".format(frequency)
