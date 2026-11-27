package com.llawsxx.audioprocess

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class ParametricEqTest {
    @Test
    fun filterTypesMatchNativeIds() {
        assertEquals((0..6).toList(), EqFilterType.entries.map { it.id })
        assertEquals(EqFilterType.BELL, EqFilterType.fromId(-1))
        assertEquals(EqFilterType.BELL, EqFilterType.fromId(99))
    }

    @Test
    fun defaultBandsAreFlat() {
        assertTrue(defaultEqBands().isEmpty())
        assertTrue(eqResponse(defaultEqBands(), 48000).total.all { abs(it) < .0001f })
    }

    @Test
    fun logarithmicFrequencyPositionsRoundTrip() {
        listOf(8000, 16000, 44100, 48000, 192000).forEach { rate ->
            val maximum = eqMaximumFrequency(rate)
            listOf(20f, 80f, 400f, 1000f, maximum).forEach { frequency ->
                assertEquals(frequency, eqFrequencyAtPosition(eqFrequencyPosition(frequency, maximum), maximum), .02f)
            }
            assertTrue(maximum < rate / 2f)
            assertEquals(20f, eqFrequencyAtPosition(-1f, maximum), 0f)
            assertEquals(maximum, eqFrequencyAtPosition(2f, maximum), .01f)
        }
    }

    @Test
    fun bellAndShelfResponsesMatchGain() {
        val band = EqBandSettings(1000f, 6f)
        assertEquals(6.0, eqCoefficients(band, 48000).responseDb(1000.0, 48000), .00001)
        val lowShelf = eqCoefficients(band.copy(type = EqFilterType.LOW_SHELF), 48000)
        val highShelf = eqCoefficients(band.copy(type = EqFilterType.HIGH_SHELF), 48000)
        assertEquals(6.0, lowShelf.responseDb(0.0, 48000), .00001)
        assertEquals(0.0, lowShelf.responseDb(24000.0, 48000), .00001)
        assertEquals(0.0, highShelf.responseDb(0.0, 48000), .00001)
        assertEquals(6.0, highShelf.responseDb(24000.0, 48000), .00001)
    }

    @Test
    fun passAndNotchResponsesHaveCorrectShapes() {
        val band = EqBandSettings(1000f)
        val highPass = eqCoefficients(band.copy(type = EqFilterType.HIGH_PASS), 48000)
        val lowPass = eqCoefficients(band.copy(type = EqFilterType.LOW_PASS), 48000)
        val notch = eqCoefficients(band.copy(type = EqFilterType.NOTCH), 48000)
        val bandPass = eqCoefficients(band.copy(type = EqFilterType.BAND_PASS), 48000)
        assertTrue(highPass.responseDb(0.0, 48000) < -100.0)
        assertEquals(-3.0103, highPass.responseDb(1000.0, 48000), .0001)
        assertEquals(0.0, highPass.responseDb(24000.0, 48000), .00001)
        assertTrue(lowPass.responseDb(24000.0, 48000) < -100.0)
        assertEquals(-3.0103, lowPass.responseDb(1000.0, 48000), .0001)
        assertEquals(0.0, lowPass.responseDb(0.0, 48000), .00001)
        assertTrue(notch.responseDb(1000.0, 48000) < -100.0)
        assertTrue(bandPass.responseDb(0.0, 48000) < -100.0)
        assertEquals(0.0, bandPass.responseDb(1000.0, 48000), .00001)
    }

    @Test
    fun totalResponseAddsOnlyEnabledBands() {
        val band = EqBandSettings(1000f, 6f)
        val enabled = eqResponse(listOf(band), 48000)
        val combined = eqResponse(listOf(band, band, band.copy(enabled = false)), 48000)
        enabled.total.indices.forEach { point ->
            assertEquals(enabled.total[point] * 2f, combined.total[point], .0001f)
        }
        assertTrue(eqResponse(listOf(band.copy(enabled = false)), 48000).total.all { it == 0f })
    }

    @Test
    fun responseCurveIncludesTheExactNotchCenter() {
        val response = eqResponse(listOf(EqBandSettings(1234f, type = EqFilterType.NOTCH, quality = 20f)), 48000)
        assertTrue(response.total.min() < -100f)
        assertTrue(response.positions.asList().zipWithNext().all { (first, second) -> second > first })
    }

    @Test
    fun invalidSettingsAreNormalized() {
        val invalid = EqBandSettings(Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY).normalized()
        assertTrue(invalid.frequency.isFinite() && invalid.gain.isFinite() && invalid.quality.isFinite())
        assertEquals(EqBandSettings(20f, 24f, .1f), EqBandSettings(-1f, 99f, -1f).normalized())
    }

    @Test
    fun nativeParameterLayoutIsFrequencyGainQualityTypeEnabled() {
        listOf(0, 1, 4, 7, 64, 257).forEach { count ->
            val bands = List(count) { index ->
                EqBandSettings(100f + index, (index % 24).toFloat(), 1f, EqFilterType.entries[index % EqFilterType.entries.size], index % 2 == 0)
            }
            val values = eqNativeValues(bands)
            assertEquals(count * 5, values.size)
            bands.forEachIndexed { index, band ->
                assertArrayEquals(
                    floatArrayOf(band.frequency, band.gain, band.quality, band.type.id.toFloat(), if (band.enabled) 1f else 0f),
                    values.copyOfRange(index * 5, index * 5 + 5), 0f
                )
            }
        }
    }

    @Test
    fun arbitraryBandCountsContributeToTheResponse() {
        listOf(0, 1, 5, 19, 64).forEach { count ->
            val bands = List(count) { EqBandSettings(1000f, 1f) }
            val response = eqResponse(bands, 48000)
            assertEquals(count, response.bands.size)
            val center = response.positions.indices.minBy { abs(response.positions[it] - eqFrequencyPosition(1000f, 20000f)) }
            assertEquals(count.toFloat(), response.total[center], .001f)
        }
    }

    @Test
    fun allResponsesStayFiniteAtSupportedExtremes() {
        listOf(8000, 48000, 192000, 384000).forEach { rate ->
            EqFilterType.entries.forEach { type ->
                listOf(20f, 20000f).forEach { frequency ->
                    listOf(.1f, 20f).forEach { quality ->
                        listOf(-24f, 24f).forEach { gain ->
                            val band = EqBandSettings(frequency, gain, quality, type)
                            assertTrue(eqResponse(listOf(band), rate).total.all { it.isFinite() })
                        }
                    }
                }
            }
        }
    }
}
