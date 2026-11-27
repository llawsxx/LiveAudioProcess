package com.llawsxx.audioprocess

data class HarmonicExciterSettings(
    val frequency: Float = 3000f,
    val drive: Float = 12f,
    val amount: Float = 15f,
    val tone: Float = 16000f,
    val even: Float = 25f,
    val outputGain: Float = 0f
) {
    fun normalized(): HarmonicExciterSettings {
        val start = finite(frequency, 3000f).coerceIn(20f, 12000f)
        return copy(
            frequency = start,
            drive = finite(drive, 12f).coerceIn(0f, 24f),
            amount = finite(amount, 15f).coerceIn(0f, 100f),
            tone = finite(tone, 16000f).coerceIn(maxOf(500f, start), 20000f),
            even = finite(even, 25f).coerceIn(0f, 100f),
            outputGain = finite(outputGain, 0f).coerceIn(-12f, 6f)
        )
    }

    fun nativeValues(): FloatArray = normalized().let {
        floatArrayOf(it.frequency, it.drive, it.amount, it.tone, it.even, it.outputGain)
    }

    fun effectiveFrequency(sampleRate: Int): Float = minOf(normalized().frequency, validRate(sampleRate) * .3f)

    fun effectiveTone(sampleRate: Int): Float = minOf(normalized().tone, validRate(sampleRate) * .45f)

    private fun finite(value: Float, fallback: Float): Float = if (value.isFinite()) value else fallback

    private fun validRate(sampleRate: Int): Int = if (sampleRate > 0) sampleRate else 48000
}
