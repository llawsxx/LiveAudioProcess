package com.llawsxx.audioprocess

import android.content.SharedPreferences
import java.lang.reflect.Proxy
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class EffectSettingsTest {
    @Test
    fun graphicalEqSettingsRoundTripForEveryFilterType() {
        EqFilterType.entries.forEach { type ->
            listOf(0, 1, 7, 64, 257).forEach { count ->
                val preferences = memoryPreferences()
                val settings = EffectSettings(
                    dspEnabled = true,
                    eqBands = List(count) { index ->
                        EqBandSettings(type = type, frequency = 150f + index, gain = (index % 12) * 2f, quality = (index % 19) + .3f, enabled = index % 2 == 0)
                    },
                    reverbDecay = 2.5f, loudnessTarget = -20f
                )
                settings.save(preferences)
                assertEquals(settings, EffectSettings.load(preferences))
            }
        }
    }

    @Test
    fun oldEqKeysAreNotMigrated() {
        val preferences = memoryPreferences(mutableMapOf("eqFrequency" to 999f, "eqGain" to 12f, "eq2Q" to 9f))
        assertEquals(defaultEqBands(), EffectSettings.load(preferences).eqBands)
    }

    @Test
    fun editingOneBandPreservesOtherEffectsAndBands() {
        val settings = EffectSettings(eqBands = List(7) { EqBandSettings(frequency = 100f * (it + 1)) }, reverbDecay = 3f, limiterThreshold = -3f)
        val replacement = EqBandSettings(200f, -6f, 2f, EqFilterType.LOW_SHELF, false)
        val updated = settings.withEqBand(2, replacement)
        assertEquals(replacement, updated.eqBands[2])
        assertEquals(settings.eqBands[0], updated.eqBands[0])
        assertEquals(settings.eqBands[3], updated.eqBands[3])
        assertEquals(settings.reverbDecay, updated.reverbDecay)
        assertEquals(settings.limiterThreshold, updated.limiterThreshold)
        assertTrue(settings.eqBands[2].enabled)
        assertFalse(updated.eqBands[2].enabled)
    }

    @Test
    fun addingAndRemovingBandsHasNoFixedCountAndCanReturnToEmpty() {
        val initial = EffectSettings(reverbDecay = 3f)
        var settings = initial
        repeat(257) { index -> settings = settings.addEqBand(EqBandSettings(frequency = 100f + index)) }
        assertEquals(257, settings.eqBands.size)
        assertEquals(initial.reverbDecay, settings.reverbDecay)
        val remaining = settings.removeEqBand(128)
        assertEquals(settings.eqBands.take(128) + settings.eqBands.drop(129), remaining.eqBands)
        repeat(257) { settings = settings.removeEqBand(0) }
        assertEquals(initial, settings)
    }

    @Test
    fun savingFewerBandsRemovesStalePreferencesAndPersistsEmpty() {
        val values = mutableMapOf<String, Any>()
        val preferences = memoryPreferences(values)
        val full = EffectSettings(eqBands = List(9) { EqBandSettings(frequency = 200f + it) })
        full.save(preferences)
        val reduced = full.removeEqBand(3).removeEqBand(7)
        reduced.save(preferences)
        assertEquals(reduced, EffectSettings.load(preferences))
        assertFalse(values.containsKey("graphicEq.7.frequency"))
        assertFalse(values.containsKey("graphicEq.8.enabled"))
        reduced.copy(eqBands = emptyList()).save(preferences)
        assertTrue(EffectSettings.load(preferences).eqBands.isEmpty())
        assertFalse(values.containsKey("graphicEq.0.frequency"))
    }

    private fun memoryPreferences(values: MutableMap<String, Any> = mutableMapOf()): SharedPreferences {
        val editor = Proxy.newProxyInstance(
            SharedPreferences.Editor::class.java.classLoader, arrayOf(SharedPreferences.Editor::class.java)
        ) { proxy, method, arguments ->
            when {
                method.name.startsWith("put") -> {
                    values[arguments[0] as String] = arguments[1]
                    proxy
                }
                method.name == "apply" -> null
                method.name == "remove" -> {
                    values.remove(arguments[0] as String)
                    proxy
                }
                else -> error("Unexpected editor method: ${method.name}")
            }
        }
        return Proxy.newProxyInstance(
            SharedPreferences::class.java.classLoader, arrayOf(SharedPreferences::class.java)
        ) { _, method, arguments ->
            when (method.name) {
                "edit" -> editor
                "getFloat", "getBoolean", "getInt" -> values[arguments[0] as String] ?: arguments[1]
                else -> error("Unexpected preferences method: ${method.name}")
            }
        } as SharedPreferences
    }
}
