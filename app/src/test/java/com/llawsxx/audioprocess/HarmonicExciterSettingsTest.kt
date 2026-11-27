package com.llawsxx.audioprocess

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class HarmonicExciterSettingsTest {
    @Test
    fun defaultsAreConservativeAndDisabled() {
        val settings = EffectSettings()
        assertFalse(settings.exciterEnabled)
        assertEquals(HarmonicExciterSettings(), settings.exciter)
        assertEquals(settings.exciter, settings.exciter.normalized())
    }

    @Test
    fun nativeLayoutIsFrequencyDriveAmountToneEvenOutput() {
        val settings = HarmonicExciterSettings(2500f, 18f, 20f, 14000f, 60f, -3f)
        assertArrayEquals(floatArrayOf(2500f, 18f, 20f, 14000f, 60f, -3f), settings.nativeValues(), 0f)
    }

    @Test
    fun limitsAndFrequencyOrderAreNormalized() {
        val settings = HarmonicExciterSettings(99999f, 99f, -1f, 1f, 999f, -99f).normalized()
        assertEquals(HarmonicExciterSettings(12000f, 24f, 0f, 12000f, 100f, -12f), settings)
        assertEquals(settings, settings.normalized())
    }

    @Test
    fun nonFiniteParametersFallBackToDefaults() {
        val settings = HarmonicExciterSettings(Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY,
            Float.NaN, Float.NaN, Float.NaN).normalized()
        assertEquals(HarmonicExciterSettings(), settings)
        assertTrue(settings.nativeValues().all { it.isFinite() })
    }

    @Test
    fun effectiveCutoffsRespectSampleRate() {
        val settings = HarmonicExciterSettings(frequency = 12000f, tone = 20000f)
        assertEquals(2400f, settings.effectiveFrequency(8000), .01f)
        assertEquals(3600f, settings.effectiveTone(8000), .01f)
        assertEquals(12000f, settings.effectiveFrequency(48000), .01f)
        assertEquals(20000f, settings.effectiveTone(48000), .01f)
        assertEquals(settings.effectiveFrequency(48000), settings.effectiveFrequency(0), .01f)
        assertEquals(settings.effectiveTone(48000), settings.effectiveTone(0), .01f)
    }
}
