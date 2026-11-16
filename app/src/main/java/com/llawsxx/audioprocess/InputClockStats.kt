package com.llawsxx.audioprocess

enum class InputClockStatus(val code: Int) {
    DISABLED(0), NO_INPUT(1), NO_OUTPUT(2), MEASURING(3), ACTIVE(4), WAITING(5)
}

data class InputClockStats(
    val status: InputClockStatus,
    val inputRate: Int,
    val estimatedRateHz: Double?,
    val outputRate: Int,
    val driftPpm: Double,
    val appliedPpm: Double,
    val windowSeconds: Double,
    val inputClockSource: Int,
    val outputClockSource: Int
) {
    companion object {
        fun fromNative(info: LongArray): InputClockStats = InputClockStats(
            status = InputClockStatus.entries.firstOrNull { it.code.toLong() == info.getOrElse(0) { 5L } }
                ?: InputClockStatus.WAITING,
            inputRate = info.getOrElse(1) { 0L }.toInt(),
            estimatedRateHz = info.getOrElse(2) { 0L }.takeIf { it > 0L }?.div(1000.0),
            outputRate = info.getOrElse(3) { 0L }.toInt(),
            driftPpm = info.getOrElse(4) { 0L } / 1000.0,
            appliedPpm = info.getOrElse(5) { 0L } / 1000.0,
            windowSeconds = info.getOrElse(6) { 0L } / 1000.0,
            inputClockSource = info.getOrElse(7) { 0L }.toInt(),
            outputClockSource = info.getOrElse(8) { 0L }.toInt()
        )
    }
}
