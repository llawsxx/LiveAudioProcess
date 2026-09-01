package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun WifiAudioPanel(role: Int, host: String, port: String, minBuffer: String, maxBuffer: String, inputTimeout: String, active: Boolean, onRole: (Int) -> Unit, onHost: (String) -> Unit, onPort: (String) -> Unit, onMinBuffer: (String) -> Unit, onMaxBuffer: (String) -> Unit, onInputTimeout: (String) -> Unit, onToggle: () -> Unit) {
    val fieldColors = OutlinedTextFieldDefaults.colors(focusedTextColor = Color.White, unfocusedTextColor = Color.White, focusedLabelColor = Color(0xFF43D5C1), unfocusedLabelColor = Color(0xFF8EA0A8), cursorColor = Color(0xFF43D5C1), focusedBorderColor = Color(0xFF43D5C1), unfocusedBorderColor = Color(0xFF4A5A60))
    Card(colors = CardDefaults.cardColors(containerColor = Color(0xFF182126)), shape = RoundedCornerShape(8.dp), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(9.dp)) {
            Text("Wi-Fi 实时音频", color = Color.White)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(selected = role == 1, onClick = { onRole(1) }, label = { Text("发送端") })
                FilterChip(selected = role == 2, onClick = { onRole(2) }, label = { Text("接收端") })
            }
            Text("编码：PCM Float32", color = Color(0xFF43D5C1), fontSize = 11.sp)
            OutlinedTextField(host, onHost, label = { Text(if (role == 1) "客户端 IP" else "本机监听 IP") }, singleLine = true, colors = fieldColors, modifier = Modifier.fillMaxWidth())
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                OutlinedTextField(port, onPort, label = { Text("端口") }, singleLine = true, colors = fieldColors, modifier = Modifier.weight(1f))
                OutlinedTextField(minBuffer, onMinBuffer, label = { Text("最小缓冲 ms (0-200)") }, singleLine = true, colors = fieldColors, modifier = Modifier.weight(1f))
            }
            OutlinedTextField(maxBuffer, onMaxBuffer, label = { Text("最大缓冲 ms (50-1000)") }, singleLine = true, colors = fieldColors, modifier = Modifier.fillMaxWidth())
            if (role == 2) OutlinedTextField(inputTimeout, onInputTimeout, label = { Text("无数据回退时间 s (0.1-60)") }, singleLine = true, colors = fieldColors, modifier = Modifier.fillMaxWidth())
            Text("接收端在最小/最大缓冲中点开始播放；低于最小值暂停，高于最大值丢弃多余采样。", color = Color(0xFF8EA0A8), fontSize = 10.sp)
            if (role == 2) Text("超时后临时使用默认麦克风；收到有效 Wi-Fi 音频后自动切回。", color = Color(0xFF8EA0A8), fontSize = 10.sp)
            Text("数据包包含序号、采样率、帧数和单调时钟时间戳；PCM 延迟最低。", color = Color(0xFF8EA0A8), fontSize = 10.sp)
            Button(onClick = onToggle, modifier = Modifier.fillMaxWidth(), colors = ButtonDefaults.buttonColors(containerColor = if (active) Color(0xFFFF6B6B) else Color(0xFF43D5C1), contentColor = Color(0xFF101417))) {
                Text(if (active) "停止 Wi-Fi 传输" else "启用 Wi-Fi 传输")
            }
        }
    }
}
