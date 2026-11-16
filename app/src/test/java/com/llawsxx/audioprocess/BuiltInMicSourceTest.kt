package com.llawsxx.audioprocess

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class BuiltInMicSourceTest {
    @Test
    fun presetsMatchAAudioValues() {
        assertEquals(0, BuiltInMicSource.DEFAULT.preset)
        assertEquals(1, BuiltInMicSource.MIC.preset)
        assertEquals(5, BuiltInMicSource.CAMCORDER.preset)
        assertEquals(6, BuiltInMicSource.VOICE_RECOGNITION.preset)
        assertEquals(7, BuiltInMicSource.VOICE_COMMUNICATION.preset)
        assertEquals(9, BuiltInMicSource.UNPROCESSED.preset)
        assertEquals(10, BuiltInMicSource.VOICE_PERFORMANCE.preset)
    }

    @Test
    fun androidEightKeepsOnlyDefault() {
        assertEquals(listOf(BuiltInMicSource.DEFAULT), BuiltInMicSource.supported(26))
        assertEquals(listOf(BuiltInMicSource.DEFAULT), BuiltInMicSource.supported(27))
    }

    @Test
    fun androidNineSupportsCustomPresetsExceptVoicePerformance() {
        val sources = BuiltInMicSource.supported(28)
        assertEquals(6, sources.size)
        assertTrue(BuiltInMicSource.MIC in sources)
        assertTrue(BuiltInMicSource.CAMCORDER in sources)
        assertTrue(BuiltInMicSource.UNPROCESSED in sources)
        assertFalse(BuiltInMicSource.VOICE_PERFORMANCE in sources)
    }

    @Test
    fun androidTenSupportsAllPresets() {
        assertEquals(BuiltInMicSource.entries.toList(), BuiltInMicSource.supported(29))
    }

    @Test
    fun savedSelectionRoundTrips() {
        BuiltInMicSource.entries.forEach { source ->
            assertEquals(source, BuiltInMicSource.fromName(source.name, 29))
        }
    }

    @Test
    fun missingInvalidOrUnsupportedSelectionFallsBackToDefault() {
        assertEquals(BuiltInMicSource.DEFAULT, BuiltInMicSource.fromName(null, 29))
        assertEquals(BuiltInMicSource.DEFAULT, BuiltInMicSource.fromName("unknown", 29))
        assertEquals(BuiltInMicSource.DEFAULT, BuiltInMicSource.fromName("MIC", 26))
        assertEquals(BuiltInMicSource.DEFAULT, BuiltInMicSource.fromName("VOICE_PERFORMANCE", 28))
    }

    @Test
    fun actualPresetResolvesToSource() {
        BuiltInMicSource.entries.forEach { source ->
            assertEquals(source, BuiltInMicSource.fromPreset(source.preset))
        }
        assertNull(BuiltInMicSource.fromPreset(-1))
        assertNull(BuiltInMicSource.fromPreset(1999))
    }
}
