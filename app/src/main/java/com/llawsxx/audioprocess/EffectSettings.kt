package com.llawsxx.audioprocess

import android.content.SharedPreferences

data class EffectSettings(
    val dspEnabled: Boolean = true, val eqEnabled: Boolean = true, val reverbEnabled: Boolean = true, val limiterEnabled: Boolean = true, val loudnessEnabled: Boolean = true,
    val eqFrequency: Float = 1200f, val eqGain: Float = 2f, val eqQ: Float = .85f,
    val eq2Frequency: Float = 250f, val eq2Gain: Float = 0f, val eq2Q: Float = 1f,
    val eq3Frequency: Float = 4000f, val eq3Gain: Float = 0f, val eq3Q: Float = 1f,
    val eq4Frequency: Float = 10000f, val eq4Gain: Float = 0f, val eq4Q: Float = 1f,
    val reverbRoom: Float = 42f, val reverbDecay: Float = 1.8f, val reverbDamping: Float = 35f, val reverbMix: Float = 18f,
    val limiterInputGain: Float = 0f, val limiterThreshold: Float = -.5f, val limiterRelease: Float = 80f, val limiterCeiling: Float = -.5f, val limiterLookAhead: Float = 1f,
    val loudnessTarget: Float = -16f, val loudnessLra: Float = 7f, val loudnessTruePeak: Float = -1f
) {
    fun save(p: SharedPreferences) = p.edit()
        .putBoolean("dspEnabled", dspEnabled).putBoolean("eqEnabled", eqEnabled).putBoolean("reverbEnabled", reverbEnabled).putBoolean("limiterEnabled", limiterEnabled).putBoolean("loudnessEnabled", loudnessEnabled)
        .putFloat("eqFrequency", eqFrequency).putFloat("eqGain", eqGain).putFloat("eqQ", eqQ)
        .putFloat("eq2Frequency", eq2Frequency).putFloat("eq2Gain", eq2Gain).putFloat("eq2Q", eq2Q)
        .putFloat("eq3Frequency", eq3Frequency).putFloat("eq3Gain", eq3Gain).putFloat("eq3Q", eq3Q)
        .putFloat("eq4Frequency", eq4Frequency).putFloat("eq4Gain", eq4Gain).putFloat("eq4Q", eq4Q)
        .putFloat("reverbRoom", reverbRoom).putFloat("reverbDecay", reverbDecay).putFloat("reverbDamping", reverbDamping).putFloat("reverbMix", reverbMix)
        .putFloat("limiterInputGain", limiterInputGain).putFloat("limiterThreshold", limiterThreshold).putFloat("limiterRelease", limiterRelease).putFloat("limiterCeiling", limiterCeiling).putFloat("limiterLookAhead", limiterLookAhead)
        .putFloat("loudnessTarget", loudnessTarget).putFloat("loudnessLra", loudnessLra).putFloat("loudnessTruePeak", loudnessTruePeak).apply()

    companion object {
        fun load(p: SharedPreferences) = EffectSettings(
            p.getBoolean("dspEnabled", true), p.getBoolean("eqEnabled", true), p.getBoolean("reverbEnabled", true), p.getBoolean("limiterEnabled", true), p.getBoolean("loudnessEnabled", true),
            p.getFloat("eqFrequency", 1200f), p.getFloat("eqGain", 2f), p.getFloat("eqQ", .85f),
            p.getFloat("eq2Frequency", 250f), p.getFloat("eq2Gain", 0f), p.getFloat("eq2Q", 1f),
            p.getFloat("eq3Frequency", 4000f), p.getFloat("eq3Gain", 0f), p.getFloat("eq3Q", 1f),
            p.getFloat("eq4Frequency", 10000f), p.getFloat("eq4Gain", 0f), p.getFloat("eq4Q", 1f),
            p.getFloat("reverbRoom", 42f), p.getFloat("reverbDecay", 1.8f), p.getFloat("reverbDamping", 35f), p.getFloat("reverbMix", 18f),
            p.getFloat("limiterInputGain", 0f), p.getFloat("limiterThreshold", -.5f), p.getFloat("limiterRelease", 80f), p.getFloat("limiterCeiling", -.5f), p.getFloat("limiterLookAhead", 1f),
            p.getFloat("loudnessTarget", -16f), p.getFloat("loudnessLra", 7f), p.getFloat("loudnessTruePeak", -1f)
        )
    }
}
