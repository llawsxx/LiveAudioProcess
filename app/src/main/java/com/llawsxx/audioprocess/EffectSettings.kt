package com.llawsxx.audioprocess

import android.content.SharedPreferences

data class EffectSettings(
    val dspEnabled: Boolean = false, val eqEnabled: Boolean = true, val reverbEnabled: Boolean = true, val limiterEnabled: Boolean = true, val loudnessEnabled: Boolean = true,
    val eqBands: List<EqBandSettings> = defaultEqBands(),
    val reverbRoom: Float = 42f, val reverbDecay: Float = 1.8f, val reverbDamping: Float = 35f, val reverbMix: Float = 18f,
    val limiterInputGain: Float = 0f, val limiterThreshold: Float = -.5f, val limiterRelease: Float = 80f, val limiterCeiling: Float = -.5f, val limiterLookAhead: Float = 1f, val limiterAdaptiveRelease: Boolean = false,
    val loudnessTarget: Float = -16f, val loudnessLra: Float = 7f, val loudnessTruePeak: Float = -1f,
    val loudnessBoostOnly: Boolean = false, val loudnessLookAhead: Float = 5f,
    val loudnessUpdateIntervalMs: Float = 1000f
) {
    fun withEqBand(index: Int, band: EqBandSettings): EffectSettings {
        require(index in eqBands.indices)
        return copy(eqBands = eqBands.mapIndexed { position, current -> if (position == index) band.normalized() else current })
    }

    fun addEqBand(band: EqBandSettings = EqBandSettings()): EffectSettings = copy(eqBands = eqBands + band.normalized())

    fun removeEqBand(index: Int): EffectSettings {
        require(index in eqBands.indices)
        return copy(eqBands = eqBands.filterIndexed { position, _ -> position != index })
    }

    fun save(p: SharedPreferences) {
        val previousBandCount = p.getInt("graphicEq.count", 0)
        val editor = p.edit()
            .putBoolean("dspEnabled", dspEnabled).putBoolean("eqEnabled", eqEnabled).putBoolean("reverbEnabled", reverbEnabled).putBoolean("limiterEnabled", limiterEnabled).putBoolean("loudnessEnabled", loudnessEnabled)
            .putFloat("reverbRoom", reverbRoom).putFloat("reverbDecay", reverbDecay).putFloat("reverbDamping", reverbDamping).putFloat("reverbMix", reverbMix)
            .putFloat("limiterInputGain", limiterInputGain).putFloat("limiterThreshold", limiterThreshold).putFloat("limiterRelease", limiterRelease).putFloat("limiterCeiling", limiterCeiling).putFloat("limiterLookAhead", limiterLookAhead).putBoolean("limiterAdaptiveRelease", limiterAdaptiveRelease)
            .putFloat("loudnessTarget", loudnessTarget).putFloat("loudnessLra", loudnessLra).putFloat("loudnessTruePeak", loudnessTruePeak).putBoolean("loudnessBoostOnly", loudnessBoostOnly).putFloat("loudnessLookAhead", loudnessLookAhead).putFloat("loudnessUpdateIntervalMs", loudnessUpdateIntervalMs)
        editor.putInt("graphicEq.count", eqBands.size)
        for (index in eqBands.size until previousBandCount) {
            listOf("frequency", "gain", "quality", "type", "enabled").forEach { key ->
                editor.remove("graphicEq.$index.$key")
            }
        }
        eqBands.forEachIndexed { index, value ->
            val band = value.normalized()
            val prefix = "graphicEq.$index."
            editor.putFloat(prefix + "frequency", band.frequency)
                .putFloat(prefix + "gain", band.gain)
                .putFloat(prefix + "quality", band.quality)
                .putInt(prefix + "type", band.type.id)
                .putBoolean(prefix + "enabled", band.enabled)
        }
        editor.apply()
    }

    companion object {
        fun load(p: SharedPreferences) = EffectSettings(
            dspEnabled = p.getBoolean("dspEnabled", false),
            eqEnabled = p.getBoolean("eqEnabled", true),
            reverbEnabled = p.getBoolean("reverbEnabled", true),
            limiterEnabled = p.getBoolean("limiterEnabled", true),
            loudnessEnabled = p.getBoolean("loudnessEnabled", true),
            eqBands = List(p.getInt("graphicEq.count", 0).coerceAtLeast(0)) { index ->
                val defaults = EqBandSettings()
                val prefix = "graphicEq.$index."
                EqBandSettings(
                    frequency = p.getFloat(prefix + "frequency", defaults.frequency),
                    gain = p.getFloat(prefix + "gain", defaults.gain),
                    quality = p.getFloat(prefix + "quality", defaults.quality),
                    type = EqFilterType.fromId(p.getInt(prefix + "type", defaults.type.id)),
                    enabled = p.getBoolean(prefix + "enabled", defaults.enabled)
                ).normalized()
            },
            reverbRoom = p.getFloat("reverbRoom", 42f), reverbDecay = p.getFloat("reverbDecay", 1.8f),
            reverbDamping = p.getFloat("reverbDamping", 35f), reverbMix = p.getFloat("reverbMix", 18f),
            limiterInputGain = p.getFloat("limiterInputGain", 0f), limiterThreshold = p.getFloat("limiterThreshold", -.5f),
            limiterRelease = p.getFloat("limiterRelease", 80f), limiterCeiling = p.getFloat("limiterCeiling", -.5f),
            limiterLookAhead = p.getFloat("limiterLookAhead", 1f).coerceIn(0f, 50f), limiterAdaptiveRelease = p.getBoolean("limiterAdaptiveRelease", false),
            loudnessTarget = p.getFloat("loudnessTarget", -16f), loudnessLra = p.getFloat("loudnessLra", 7f), loudnessTruePeak = p.getFloat("loudnessTruePeak", -1f),
            loudnessBoostOnly = p.getBoolean("loudnessBoostOnly", false), loudnessLookAhead = p.getFloat("loudnessLookAhead", 5f).coerceIn(5f, 50f),
            loudnessUpdateIntervalMs = p.getFloat("loudnessUpdateIntervalMs", 1000f).coerceIn(100f, 3000f)
        )
    }
}
