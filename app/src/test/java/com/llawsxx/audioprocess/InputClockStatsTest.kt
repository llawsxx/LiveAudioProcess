package com.llawsxx.audioprocess

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class InputClockStatsTest {
    @Test
    fun nativeUnitsConvertToHertzAndPpm() {
        val stats = InputClockStats.fromNative(longArrayOf(4, 48000, 48004800, 96000, 100000, 95000, 15750, 1, 3))
        assertEquals(InputClockStatus.ACTIVE, stats.status)
        assertEquals(48000, stats.inputRate)
        assertEquals(48004.8, stats.estimatedRateHz!!, 0.0001)
        assertEquals(96000, stats.outputRate)
        assertEquals(100.0, stats.driftPpm, 0.0001)
        assertEquals(95.0, stats.appliedPpm, 0.0001)
        assertEquals(15.75, stats.windowSeconds, 0.0001)
        assertEquals(1, stats.inputClockSource)
        assertEquals(3, stats.outputClockSource)
    }

    @Test
    fun negativeDriftIsPreserved() {
        val stats = InputClockStats.fromNative(longArrayOf(4, 48000, 47995200, 48000, -100000, -95000))
        assertEquals(-100.0, stats.driftPpm, 0.0001)
        assertEquals(-95.0, stats.appliedPpm, 0.0001)
    }

    @Test
    fun disabledCompensationCanStillDisplayMeasuredRate() {
        val stats = InputClockStats.fromNative(longArrayOf(0, 48000, 48004800, 48000, 100000, 0))
        assertEquals(InputClockStatus.DISABLED, stats.status)
        assertEquals(48004.8, stats.estimatedRateHz!!, 0.0001)
        assertEquals(0.0, stats.appliedPpm, 0.0001)
    }

    @Test
    fun noOutputHasNoMeasuredRateOrCompensation() {
        val stats = InputClockStats.fromNative(longArrayOf(2, 48000, 0, 0, 0, 0))
        assertEquals(InputClockStatus.NO_OUTPUT, stats.status)
        assertNull(stats.estimatedRateHz)
        assertEquals(0.0, stats.appliedPpm, 0.0001)
    }

    @Test
    fun incompleteTelemetryDoesNotInventASampleRate() {
        val stats = InputClockStats.fromNative(longArrayOf(3))
        assertEquals(InputClockStatus.MEASURING, stats.status)
        assertNull(stats.estimatedRateHz)
        assertEquals(0, stats.outputRate)
        assertEquals(InputClockStatus.WAITING, InputClockStats.fromNative(longArrayOf()).status)
        assertEquals(InputClockStatus.WAITING, InputClockStats.fromNative(longArrayOf(99)).status)
    }
}
