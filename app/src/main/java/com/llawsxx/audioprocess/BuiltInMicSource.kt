package com.llawsxx.audioprocess

enum class BuiltInMicSource(val preset: Int, val label: String, val description: String, val minSdk: Int) {
    DEFAULT(0, "默认（保持原有行为）", "不指定采集预设，使用 AAudio 默认配置", 26),
    MIC(1, "MIC", "通用麦克风录音", 28),
    CAMCORDER(5, "CAMCORDER", "视频录制场景，麦克风方向与处理由设备决定", 28),
    VOICE_RECOGNITION(6, "VOICE_RECOGNITION", "语音识别场景", 28),
    VOICE_COMMUNICATION(7, "VOICE_COMMUNICATION", "语音通信场景，可能启用回声消除等处理", 28),
    UNPROCESSED(9, "UNPROCESSED", "请求未经处理的输入，实际支持取决于设备，音量可能较低", 28),
    VOICE_PERFORMANCE(10, "VOICE_PERFORMANCE", "实时演唱或监听场景，优先降低采集延迟", 29);

    companion object {
        fun supported(sdkInt: Int): List<BuiltInMicSource> = entries.filter { sdkInt >= it.minSdk }

        fun fromName(name: String?, sdkInt: Int): BuiltInMicSource =
            entries.firstOrNull { it.name == name && sdkInt >= it.minSdk } ?: DEFAULT

        fun fromPreset(preset: Int): BuiltInMicSource? = entries.firstOrNull { it.preset == preset }
    }
}
