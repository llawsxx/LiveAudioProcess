package com.llawsxx.audioprocess

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

fun defaultEqBands(): List<EqBandSettings> = emptyList()

fun eqNativeValues(bands: List<EqBandSettings>): FloatArray {
    return bands.flatMap { value ->
        val band = value.normalized()
        listOf(band.frequency, band.gain, band.quality, band.type.id.toFloat(), if (band.enabled) 1f else 0f)
    }.toFloatArray()
}

enum class EqFilterType(val id: Int, val label: String, val usesGain: Boolean) {
    BELL(0, "峰值 Bell", true),
    HIGH_PASS(1, "高通 High-pass", false),
    LOW_PASS(2, "低通 Low-pass", false),
    NOTCH(3, "陷波 Notch", false),
    BAND_PASS(4, "带通 Band-pass", false),
    LOW_SHELF(5, "低搁架 Low Shelf", true),
    HIGH_SHELF(6, "高搁架 High Shelf", true);

    companion object {
        fun fromId(id: Int): EqFilterType = entries.firstOrNull { it.id == id } ?: BELL
    }
}

data class EqBandSettings(
    val frequency: Float = 1000f,
    val gain: Float = 0f,
    val quality: Float = .70710677f,
    val type: EqFilterType = EqFilterType.BELL,
    val enabled: Boolean = true
) {
    fun normalized(): EqBandSettings = copy(
        frequency = (if (frequency.isFinite()) frequency else 1000f).coerceIn(20f, 20000f),
        gain = (if (gain.isFinite()) gain else 0f).coerceIn(-24f, 24f),
        quality = (if (quality.isFinite()) quality else .70710677f).coerceIn(.1f, 20f)
    )
}

fun eqMaximumFrequency(sampleRate: Int): Float = minOf(20000f, (sampleRate.takeIf { it > 0 } ?: 48000).coerceAtLeast(1000) * .49f)

fun eqFrequencyPosition(frequency: Float, maximum: Float): Float =
    (ln(frequency.coerceIn(20f, maximum) / 20.0) / ln(maximum / 20.0)).toFloat()

fun eqFrequencyAtPosition(position: Float, maximum: Float): Float =
    (20.0 * (maximum / 20.0).pow(position.coerceIn(0f, 1f).toDouble())).toFloat().coerceIn(20f, maximum)

data class EqCoefficients(
    val numerator0: Double,
    val numerator1: Double,
    val numerator2: Double,
    val denominator1: Double,
    val denominator2: Double
) {
    fun responseDb(frequency: Double, sampleRate: Int): Double {
        val omega = 2.0 * PI * frequency / sampleRate
        val cosine = cos(omega)
        val sine = sin(omega)
        val cosine2 = cos(2.0 * omega)
        val sine2 = sin(2.0 * omega)
        val numeratorReal = numerator0 + numerator1 * cosine + numerator2 * cosine2
        val numeratorImaginary = -numerator1 * sine - numerator2 * sine2
        val denominatorReal = 1.0 + denominator1 * cosine + denominator2 * cosine2
        val denominatorImaginary = -denominator1 * sine - denominator2 * sine2
        val numeratorPower = numeratorReal * numeratorReal + numeratorImaginary * numeratorImaginary
        val denominatorPower = denominatorReal * denominatorReal + denominatorImaginary * denominatorImaginary
        return 10.0 * log10(numeratorPower.coerceAtLeast(1e-30) / denominatorPower.coerceAtLeast(1e-30))
    }
}

fun eqCoefficients(settings: EqBandSettings, sampleRate: Int): EqCoefficients {
    val band = settings.normalized()
    val rate = sampleRate.takeIf { it > 0 } ?: 48000
    val frequency = band.frequency.toDouble().coerceIn(20.0, rate * .49)
    val amplitude = 10.0.pow(band.gain / 40.0)
    val omega = 2.0 * PI * frequency / rate
    val cosine = cos(omega)
    val alpha = sin(omega) / (2.0 * band.quality)
    val shelf = 2.0 * sqrt(amplitude) * alpha
    var denominator0 = 1.0 + alpha
    var denominator1 = -2.0 * cosine
    var denominator2 = 1.0 - alpha
    val numerator = when (band.type) {
        EqFilterType.HIGH_PASS -> doubleArrayOf((1.0 + cosine) / 2.0, -(1.0 + cosine), (1.0 + cosine) / 2.0)
        EqFilterType.LOW_PASS -> doubleArrayOf((1.0 - cosine) / 2.0, 1.0 - cosine, (1.0 - cosine) / 2.0)
        EqFilterType.NOTCH -> doubleArrayOf(1.0, -2.0 * cosine, 1.0)
        EqFilterType.BAND_PASS -> doubleArrayOf(alpha, 0.0, -alpha)
        EqFilterType.LOW_SHELF -> {
            denominator0 = (amplitude + 1.0) + (amplitude - 1.0) * cosine + shelf
            denominator1 = -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine)
            denominator2 = (amplitude + 1.0) + (amplitude - 1.0) * cosine - shelf
            doubleArrayOf(
                amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + shelf),
                2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine),
                amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - shelf)
            )
        }
        EqFilterType.HIGH_SHELF -> {
            denominator0 = (amplitude + 1.0) - (amplitude - 1.0) * cosine + shelf
            denominator1 = 2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine)
            denominator2 = (amplitude + 1.0) - (amplitude - 1.0) * cosine - shelf
            doubleArrayOf(
                amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + shelf),
                -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine),
                amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - shelf)
            )
        }
        EqFilterType.BELL -> {
            denominator0 = 1.0 + alpha / amplitude
            denominator2 = 1.0 - alpha / amplitude
            doubleArrayOf(1.0 + alpha * amplitude, -2.0 * cosine, 1.0 - alpha * amplitude)
        }
    }
    return EqCoefficients(
        numerator[0] / denominator0, numerator[1] / denominator0, numerator[2] / denominator0,
        denominator1 / denominator0, denominator2 / denominator0
    )
}

data class EqResponse(val positions: FloatArray, val bands: List<FloatArray>, val total: FloatArray)

fun eqResponse(bands: List<EqBandSettings>, sampleRate: Int, points: Int = 256): EqResponse {
    require(points >= 2)
    val rate = sampleRate.takeIf { it > 0 } ?: 48000
    val maximum = eqMaximumFrequency(rate)
    val frequencies = (List(points) { eqFrequencyAtPosition(it.toFloat() / (points - 1), maximum) } +
        bands.map { it.normalized().frequency.coerceAtMost(maximum) }).distinct().sorted()
    val positions = frequencies.map { eqFrequencyPosition(it, maximum) }.toFloatArray()
    val curves = bands.map { band ->
        val coefficients = eqCoefficients(band, rate)
        FloatArray(frequencies.size) { coefficients.responseDb(frequencies[it].toDouble(), rate).toFloat() }
    }
    val total = FloatArray(frequencies.size) { point ->
        bands.indices.sumOf { band -> if (bands[band].enabled) curves[band][point].toDouble() else 0.0 }.toFloat()
    }
    return EqResponse(positions, curves, total)
}
