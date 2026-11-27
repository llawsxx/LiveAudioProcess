package com.llawsxx.audioprocess

import android.graphics.Paint
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.ln
import kotlin.math.pow

private val EqPanelColor = Color(0xFF182126)
private val EqPlotColor = Color(0xFF0C141A)
private val EqMuted = Color(0xFF8EA0A8)
private val EqAccent = Color(0xFF43D5C1)
private val EqBandColors = listOf(Color(0xFFFFBE66), Color(0xFF65D9A5), Color(0xFF62B9FF), Color(0xFFD796FF))

private fun eqBandColor(index: Int): Color = EqBandColors[index % EqBandColors.size]

@Composable
fun EqBandsPanel(settings: EffectSettings, sampleRate: Int, onChange: (EffectSettings) -> Unit) {
    var selectedBandIndex by rememberSaveable { mutableIntStateOf(0) }
    var typeMenuExpanded by remember { mutableStateOf(false) }
    val bands = settings.eqBands
    val selectedBand = selectedBandIndex.coerceIn(0, (bands.size - 1).coerceAtLeast(0))
    val band = bands.getOrNull(selectedBand)
    val bandListState = rememberLazyListState()
    LaunchedEffect(selectedBand, bands.size) {
        if (bands.isNotEmpty()) bandListState.animateScrollToItem(selectedBand)
    }
    fun addBand(value: EqBandSettings) {
        selectedBandIndex = bands.size
        onChange(settings.addEqBand(value))
    }
    val maximum = eqMaximumFrequency(sampleRate)
    val active = settings.dspEnabled && settings.eqEnabled
    Card(
        colors = CardDefaults.cardColors(containerColor = EqPanelColor),
        shape = RoundedCornerShape(8.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("图形参数均衡器", color = Color.White, fontWeight = FontWeight.Bold)
                    Text("${bands.size} BANDS · PARAMETRIC EQ", color = EqMuted, fontSize = 10.sp)
                }
                Text(if (active) "已启用" else "旁路 · 曲线预览", color = if (active) EqAccent else EqMuted, fontSize = 11.sp)
            }
            EqResponseGraph(bands, sampleRate, selectedBand, active, { selectedBandIndex = it }, { addBand(it) }) { index, value ->
                onChange(settings.withEqBand(index, value))
            }
            Text("双击空白处添加节点；拖动调频率 / 增益。白线为已开启频段的总响应。", color = EqMuted, fontSize = 10.sp)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { addBand(EqBandSettings()) }) { Text("添加节点", color = EqAccent) }
                TextButton(enabled = band != null, onClick = {
                    onChange(settings.removeEqBand(selectedBand))
                    selectedBandIndex = (selectedBand - 1).coerceAtLeast(0)
                }) { Text("删除选中", color = if (band != null) EqMuted else EqMuted.copy(alpha = .4f)) }
                TextButton(enabled = bands.isNotEmpty(), onClick = {
                    selectedBandIndex = 0
                    onChange(settings.copy(eqBands = emptyList()))
                }) { Text("清空", color = EqMuted) }
            }
            if (bands.isEmpty()) {
                Text("暂无节点，当前为平直响应。双击曲线区域或点击“添加节点”开始。", color = EqMuted, fontSize = 11.sp)
            }
            LazyRow(state = bandListState, modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                itemsIndexed(bands) { index, value ->
                    FilterChip(
                        selected = selectedBand == index,
                        onClick = { selectedBandIndex = index },
                        label = { Text("${index + 1}${if (value.enabled) "" else " · 关"}", fontSize = 12.sp) },
                        colors = FilterChipDefaults.filterChipColors(
                            labelColor = eqBandColor(index).copy(alpha = if (value.enabled) 1f else .5f),
                            selectedLabelColor = eqBandColor(index),
                            selectedContainerColor = eqBandColor(index).copy(alpha = .15f)
                        )
                    )
                }
            }
            if (band != null) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("频段 ${selectedBand + 1}", color = eqBandColor(selectedBand), fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                    Text("本段开关", color = EqMuted, fontSize = 11.sp)
                    Switch(checked = band.enabled, onCheckedChange = { onChange(settings.withEqBand(selectedBand, band.copy(enabled = it))) })
                }
                Box {
                    OutlinedButton(onClick = { typeMenuExpanded = true }, modifier = Modifier.fillMaxWidth()) {
                        Text("${band.type.label}  ▾", color = eqBandColor(selectedBand))
                    }
                    DropdownMenu(expanded = typeMenuExpanded, onDismissRequest = { typeMenuExpanded = false }) {
                        EqFilterType.entries.forEach { type ->
                            DropdownMenuItem(
                                text = { Text(type.label) },
                                onClick = {
                                    onChange(settings.withEqBand(selectedBand, band.copy(type = type)))
                                    typeMenuExpanded = false
                                }
                            )
                        }
                    }
                }
                EqParameterSlider("频率", eqFrequencyPosition(band.frequency, maximum), eqFrequencyLabel(band.frequency.coerceAtMost(maximum))) {
                    onChange(settings.withEqBand(selectedBand, band.copy(frequency = eqFrequencyAtPosition(it, maximum))))
                }
                if (band.type.usesGain) {
                    EqParameterSlider("增益", (band.gain + 24f) / 48f, "%+.1f dB".format(band.gain)) {
                        onChange(settings.withEqBand(selectedBand, band.copy(gain = it * 48f - 24f)))
                    }
                }
                EqParameterSlider("Q 值", (ln(band.quality / .1) / ln(200.0)).toFloat(), "%.2f".format(band.quality)) {
                    onChange(settings.withEqBand(selectedBand, band.copy(quality = (.1 * 200.0.pow(it.toDouble())).toFloat())))
                }
                Text(
                    when (band.type) {
                        EqFilterType.HIGH_PASS, EqFilterType.LOW_PASS -> "二阶滤波 · 12 dB/oct · Q 控制截止附近的共振，0.707 为平坦通带。"
                        EqFilterType.NOTCH -> "抑制中心频率；Q 越大，陷波越窄。节点仅左右调频率。"
                        EqFilterType.BAND_PASS -> "保留中心频率附近的声音；Q 越大，通带越窄。节点仅左右调频率。"
                        EqFilterType.LOW_SHELF, EqFilterType.HIGH_SHELF -> "提升或削弱低频 / 高频；Q 控制过渡形状，过大可能产生共振。"
                        EqFilterType.BELL -> "提升或削弱中心频率附近的声音；Q 越大，影响范围越窄。"
                    },
                    color = EqMuted, fontSize = 10.sp
                )
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Text("DSP ${sampleRate} Hz · ±24 dB", color = EqMuted, fontSize = 10.sp, modifier = Modifier.weight(1f))
                    TextButton(onClick = {
                        onChange(settings.withEqBand(selectedBand, EqBandSettings(frequency = band.frequency)))
                    }) { Text("复位本段", color = EqMuted, fontSize = 11.sp) }
                }
            }
        }
    }
}

@Composable
private fun EqParameterSlider(label: String, value: Float, readout: String, onChange: (Float) -> Unit) {
    Column {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(label, color = Color.White, fontSize = 12.sp)
            Text(readout, color = EqAccent, fontSize = 12.sp)
        }
        Slider(
            value = value.coerceIn(0f, 1f), onValueChange = onChange,
            modifier = Modifier.fillMaxWidth().height(32.dp).semantics { contentDescription = "EQ $label" }
        )
    }
}

private fun eqFrequencyLabel(frequency: Float): String =
    if (frequency >= 1000f) "%.2f kHz".format(frequency / 1000f) else "%.0f Hz".format(frequency)

private fun eqPlotBounds(width: Float, height: Float, density: Float): Rect =
    Rect(34f * density, 18f * density, width - 15f * density, height - 26f * density)

private fun eqNodePosition(band: EqBandSettings, bounds: Rect, maximum: Float): Offset = Offset(
    bounds.left + eqFrequencyPosition(band.frequency, maximum) * bounds.width,
    bounds.center.y - (if (band.type.usesGain) band.gain else 0f) * bounds.height / 48f
)

@Composable
private fun EqResponseGraph(
    bands: List<EqBandSettings>, sampleRate: Int, selectedBand: Int, active: Boolean,
    onSelect: (Int) -> Unit, onAdd: (EqBandSettings) -> Unit, onBandChange: (Int, EqBandSettings) -> Unit
) {
    val response = remember(bands, sampleRate) { eqResponse(bands, sampleRate) }
    val latestOnAdd by rememberUpdatedState(onAdd)
    val latestBands by rememberUpdatedState(bands)
    val latestSelected by rememberUpdatedState(selectedBand)
    val latestOnSelect by rememberUpdatedState(onSelect)
    val latestOnChange by rememberUpdatedState(onBandChange)
    val maximum = eqMaximumFrequency(sampleRate)
    val density = LocalDensity.current
    val labelPaint = remember(density) {
        Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = with(density) { 9.sp.toPx() }; color = android.graphics.Color.rgb(142, 160, 168) }
    }
    val nodePaint = remember(density) {
        Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = with(density) { 10.sp.toPx() }
            textAlign = Paint.Align.CENTER
            isFakeBoldText = true
        }
    }
    Canvas(
        Modifier.fillMaxWidth().height(250.dp).clip(RoundedCornerShape(8.dp)).background(EqPlotColor)
            .semantics { contentDescription = if (bands.isEmpty()) "EQ 频响曲线，暂无节点，双击添加" else "EQ 频响曲线，已选择频段 ${selectedBand + 1}" }
            .pointerInput(sampleRate, maximum) {
                var previousTapTime = 0L
                var previousTapPosition = Offset.Zero
                awaitEachGesture {
                    val down = awaitFirstDown(requireUnconsumed = false)
                    val bounds = eqPlotBounds(size.width.toFloat(), size.height.toFloat(), density.density)
                    val radius = 24.dp.toPx()
                    val candidates = latestBands.indices.filter {
                        (eqNodePosition(latestBands[it], bounds, maximum) - down.position).getDistance() <= radius
                    }
                    val hit = if (latestSelected in candidates) latestSelected else candidates.minByOrNull {
                        (eqNodePosition(latestBands[it], bounds, maximum) - down.position).getDistance()
                    }
                    if (hit != null) {
                        previousTapTime = 0L
                        latestOnSelect(hit)
                        val grabOffset = down.position - eqNodePosition(latestBands[hit], bounds, maximum)
                        down.consume()
                        drag(down.id) { change ->
                            val position = change.position - grabOffset
                            val current = latestBands.getOrNull(hit) ?: return@drag
                            val frequency = eqFrequencyAtPosition((position.x - bounds.left) / bounds.width, maximum)
                            val gain = if (current.type.usesGain) ((bounds.center.y - position.y) * 48f / bounds.height).coerceIn(-24f, 24f) else current.gain
                            latestOnChange(hit, current.copy(frequency = frequency, gain = gain))
                            change.consume()
                        }
                    } else if (bounds.contains(down.position)) {
                        val up = waitForUpOrCancellation()
                        if (up != null && up.uptimeMillis - down.uptimeMillis < viewConfiguration.longPressTimeoutMillis) {
                            val elapsed = down.uptimeMillis - previousTapTime
                            if (previousTapTime != 0L &&
                                elapsed in viewConfiguration.doubleTapMinTimeMillis..viewConfiguration.doubleTapTimeoutMillis &&
                                (down.position - previousTapPosition).getDistance() <= radius) {
                                val frequency = eqFrequencyAtPosition((up.position.x - bounds.left) / bounds.width, maximum)
                                val gain = ((bounds.center.y - up.position.y) * 48f / bounds.height).coerceIn(-24f, 24f)
                                latestOnAdd(EqBandSettings(frequency = frequency, gain = gain))
                                previousTapTime = 0L
                                up.consume()
                            } else {
                                previousTapTime = up.uptimeMillis
                                previousTapPosition = up.position
                            }
                        } else {
                            previousTapTime = 0L
                        }
                    }
                }
            }
    ) {
        val bounds = eqPlotBounds(size.width, size.height, density.density)
        val canvas = drawContext.canvas.nativeCanvas
        labelPaint.textAlign = Paint.Align.LEFT
        listOf(24, 12, 0, -12, -24).forEach { gain ->
            val vertical = bounds.center.y - gain * bounds.height / 48f
            drawLine(Color.White.copy(alpha = if (gain == 0) .25f else .08f), Offset(bounds.left, vertical), Offset(bounds.right, vertical))
            canvas.drawText(if (gain > 0) "+$gain" else "$gain", 3.dp.toPx(), vertical + 3.dp.toPx(), labelPaint)
        }
        labelPaint.textAlign = Paint.Align.CENTER
        listOf(20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000).filter { it <= maximum }.forEach { frequency ->
            val horizontal = bounds.left + eqFrequencyPosition(frequency.toFloat(), maximum) * bounds.width
            drawLine(Color.White.copy(alpha = .07f), Offset(horizontal, bounds.top), Offset(horizontal, bounds.bottom))
            if (frequency in listOf(20, 100, 1000, 10000, 20000)) {
                canvas.drawText(if (frequency >= 1000) "${frequency / 1000}k" else "$frequency", horizontal, size.height - 7.dp.toPx(), labelPaint)
            }
        }
        fun curvePath(values: FloatArray): Path = Path().apply {
            values.forEachIndexed { point, gain ->
                val horizontal = bounds.left + response.positions[point] * bounds.width
                val vertical = bounds.center.y - gain.coerceIn(-48f, 48f) * bounds.height / 48f
                if (point == 0) moveTo(horizontal, vertical) else lineTo(horizontal, vertical)
            }
        }
        clipRect(bounds.left, bounds.top, bounds.right, bounds.bottom) {
            response.bands.forEachIndexed { index, curve ->
                drawPath(
                    curvePath(curve), eqBandColor(index).copy(alpha = if (!bands[index].enabled) .15f else if (index == selectedBand) .85f else .35f),
                    style = Stroke(width = if (index == selectedBand) 1.5.dp.toPx() else 1.dp.toPx())
                )
            }
            drawPath(curvePath(response.total), Color.White.copy(alpha = if (active) .95f else .45f), style = Stroke(2.dp.toPx()))
        }
        bands.forEachIndexed { index, band ->
            val position = eqNodePosition(band, bounds, maximum)
            if (selectedBand == index) drawCircle(eqBandColor(index).copy(alpha = .2f), 16.dp.toPx(), position)
            drawCircle(if (band.enabled) eqBandColor(index) else EqPlotColor, 9.dp.toPx(), position)
            drawCircle(eqBandColor(index).copy(alpha = if (band.enabled) 1f else .5f), 9.dp.toPx(), position, style = Stroke(1.5.dp.toPx()))
            nodePaint.color = if (band.enabled) android.graphics.Color.rgb(12, 20, 26) else android.graphics.Color.LTGRAY
            canvas.drawText("${index + 1}", position.x, position.y - (nodePaint.ascent() + nodePaint.descent()) / 2f, nodePaint)
        }
    }
}
