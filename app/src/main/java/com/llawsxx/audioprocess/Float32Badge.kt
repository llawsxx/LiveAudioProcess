package com.llawsxx.audioprocess

import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun Float32Badge() {
    Row(Modifier.border(1.dp, Color(0xFF2C7770), RoundedCornerShape(8.dp)).padding(horizontal = 10.dp, vertical = 7.dp)) {
        Text("AAUDIO · FLOAT32", color = Color(0xFF43D5C1), fontSize = 10.sp)
        Spacer(Modifier.width(8.dp))
        Text("内部处理不经过 PCM16 截断 · 输出端软限幅", color = Color(0xFF8EA0A8), fontSize = 10.sp)
    }
}
