package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun EqBandsPanel(settings: EffectSettings, onChange: (EffectSettings) -> Unit) {
    val rows = listOf(
        Triple("频点 2", settings.eq2Frequency, settings.eq2Gain),
        Triple("频点 3", settings.eq3Frequency, settings.eq3Gain),
        Triple("频点 4", settings.eq4Frequency, settings.eq4Gain)
    )
    Card(colors = CardDefaults.cardColors(containerColor = Color(0xFF182126)), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
            Text("EQ 其余频点", color = Color(0xFF43D5C1), fontSize = 11.sp, fontWeight = FontWeight.Bold)
            rows.forEachIndexed { index, row ->
                val band = index + 2
                Text(row.first, color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                EqBandSlider("频率", row.second, 20f..20000f, { value -> onChange(when (band) { 2 -> settings.copy(eq2Frequency = value); 3 -> settings.copy(eq3Frequency = value); else -> settings.copy(eq4Frequency = value) }) }, "${row.second.toInt()} Hz")
                EqBandSlider("增益", row.third, -12f..12f, { value -> onChange(when (band) { 2 -> settings.copy(eq2Gain = value); 3 -> settings.copy(eq3Gain = value); else -> settings.copy(eq4Gain = value) }) }, "${row.third.toInt()} dB")
                val q = when (band) { 2 -> settings.eq2Q; 3 -> settings.eq3Q; else -> settings.eq4Q }
                EqBandSlider("Q", q, .5f..10f, { value -> onChange(when (band) { 2 -> settings.copy(eq2Q = value); 3 -> settings.copy(eq3Q = value); else -> settings.copy(eq4Q = value) }) }, "Q ${"%.2f".format(q)}")
            }
        }
    }
}

@Composable private fun EqBandSlider(label: String, value: Float, range: ClosedFloatingPointRange<Float>, onChange: (Float) -> Unit, readout: String) {
    Row(Modifier.fillMaxWidth()) { Text(label, color = Color(0xFF8EA0A8), fontSize = 10.sp, modifier = Modifier.weight(.16f)); Slider(value = value, onValueChange = onChange, valueRange = range, modifier = Modifier.weight(.68f).height(28.dp)); Spacer(Modifier.width(12.dp)); Text(readout, color = Color(0xFF43D5C1), fontSize = 10.sp, modifier = Modifier.weight(.16f)) }
}
