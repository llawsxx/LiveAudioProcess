package com.llawsxx.audioprocess

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ArrowForward
import androidx.compose.material.icons.outlined.FiberManualRecord
import androidx.compose.material.icons.outlined.PlayArrow
import androidx.compose.material.icons.outlined.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.Locale
import kotlin.math.roundToInt

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun EngineControlBar(
    running: Boolean,
    recording: Boolean,
    elapsed: Int,
    hasPermission: Boolean,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onRecordingChange: () -> Unit
) {
    val ink = Color(0xFF101417)
    val teal = Color(0xFF43D5C1)
    val red = Color(0xFFFF6B6B)
    TopAppBar(
        title = {
            Row(
                Modifier.fillMaxWidth().padding(end = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                if (running) {
                    SlideToStopEngine(onStop = onStop, modifier = Modifier.weight(1f))
                } else {
                    Button(
                        onClick = onStart,
                        modifier = Modifier.weight(1f).height(52.dp),
                        colors = ButtonDefaults.buttonColors(containerColor = teal, contentColor = ink),
                        shape = RoundedCornerShape(10.dp)
                    ) {
                        Icon(Icons.Outlined.PlayArrow, null)
                        Spacer(Modifier.width(8.dp))
                        Text(if (hasPermission) "开始引擎" else "授权麦克风", fontWeight = FontWeight.Bold)
                    }
                }
                Button(
                    onClick = onRecordingChange,
                    enabled = running,
                    modifier = Modifier.width(104.dp).height(52.dp),
                    contentPadding = PaddingValues(horizontal = 8.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (recording) red.copy(alpha = .18f) else Color(0xFF182126),
                        contentColor = if (recording) red else Color.White
                    ),
                    shape = RoundedCornerShape(10.dp)
                ) {
                    Icon(if (recording) Icons.Outlined.Stop else Icons.Outlined.FiberManualRecord, null)
                    Spacer(Modifier.width(5.dp))
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(if (recording) "停止录音" else "录音", fontSize = 12.sp, maxLines = 1)
                        if (recording) {
                            Text(String.format(Locale.US, "%02d:%02d", elapsed / 60, elapsed % 60), fontSize = 10.sp)
                        }
                    }
                }
            }
        },
        colors = TopAppBarDefaults.topAppBarColors(containerColor = ink)
    )
}

@Composable
internal fun SlideToStopEngine(onStop: () -> Unit, modifier: Modifier = Modifier) {
    val latestOnStop by rememberUpdatedState(onStop)
    BoxWithConstraints(
        modifier.height(52.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(Color(0xFF202D33))
            .semantics { contentDescription = "引擎停止滑轨" }
            .padding(2.dp)
    ) {
        val travelPx = with(LocalDensity.current) { (maxWidth - 48.dp).toPx().coerceAtLeast(0f) }
        var offsetPx by remember(travelPx) { mutableFloatStateOf(0f) }
        var submitted by remember(travelPx) { mutableStateOf(false) }
        Text(
            if (submitted) "正在停止…" else "右滑停止引擎",
            color = Color.White,
            fontSize = 12.sp,
            maxLines = 1,
            modifier = Modifier.align(Alignment.Center).padding(start = 44.dp, end = 4.dp)
        )
        Box(
            Modifier.offset { IntOffset(offsetPx.roundToInt(), 0) }
                .size(48.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFFFF6B6B))
                .semantics { contentDescription = "停止引擎滑块" }
                .pointerInput(travelPx) {
                    detectHorizontalDragGestures(
                        onDragEnd = {
                            if (!submitted && travelPx > 0f && offsetPx >= travelPx * .95f) {
                                submitted = true
                                offsetPx = travelPx
                                latestOnStop()
                            } else if (!submitted) {
                                offsetPx = 0f
                            }
                        },
                        onDragCancel = { if (!submitted) offsetPx = 0f },
                        onHorizontalDrag = { change, distance ->
                            change.consume()
                            if (!submitted) offsetPx = (offsetPx + distance).coerceIn(0f, travelPx)
                        }
                    )
                },
            contentAlignment = Alignment.Center
        ) {
            Icon(Icons.Outlined.ArrowForward, null, tint = Color(0xFF101417))
        }
    }
}
