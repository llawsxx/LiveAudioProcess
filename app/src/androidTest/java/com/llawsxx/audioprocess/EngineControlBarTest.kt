package com.llawsxx.audioprocess

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.click
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithContentDescription
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.compose.ui.test.performTouchInput
import androidx.compose.ui.test.swipe
import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test

class EngineControlBarTest {
    @get:Rule
    val composeRule = createComposeRule()

    private var stopCount = 0

    private fun showSlider() {
        composeRule.setContent {
            MaterialTheme {
                Box(Modifier.width(320.dp)) {
                    SlideToStopEngine(onStop = { stopCount++ }, modifier = Modifier.fillMaxWidth())
                }
            }
        }
    }

    @Test
    fun tappingThumbOrTrackDoesNotStopEngine() {
        showSlider()
        composeRule.onNodeWithContentDescription("停止引擎滑块").performTouchInput { click() }
        composeRule.onNodeWithContentDescription("引擎停止滑轨").performTouchInput {
            click(Offset(width - 5f, centerY))
        }
        composeRule.runOnIdle { assertEquals(0, stopCount) }
    }

    @Test
    fun partialDragReturnsToStartWithoutStopping() {
        showSlider()
        val thumb = composeRule.onNodeWithContentDescription("停止引擎滑块")
        val initialLeft = thumb.fetchSemanticsNode().boundsInRoot.left
        composeRule.onNodeWithContentDescription("引擎停止滑轨").performTouchInput {
            swipe(Offset(26.dp.toPx(), centerY), Offset(width * .55f, centerY), 400)
        }
        composeRule.waitForIdle()
        assertEquals(initialLeft, thumb.fetchSemanticsNode().boundsInRoot.left, .1f)
        composeRule.runOnIdle { assertEquals(0, stopCount) }
    }

    @Test
    fun fullDragStopsOnlyAfterReleaseAndOnlyOnce() {
        showSlider()
        val track = composeRule.onNodeWithContentDescription("引擎停止滑轨")
        track.performTouchInput {
            down(Offset(26.dp.toPx(), centerY))
            moveTo(Offset(width - 2f, centerY), 400)
        }
        composeRule.runOnIdle { assertEquals(0, stopCount) }
        track.performTouchInput { up() }
        composeRule.runOnIdle { assertEquals(1, stopCount) }
        composeRule.onNodeWithContentDescription("停止引擎滑块").performTouchInput {
            swipe(center, Offset(centerX - 100f, centerY), 400)
        }
        composeRule.runOnIdle { assertEquals(1, stopCount) }
    }

    @Test
    fun cancelledDragReturnsToStartWithoutStopping() {
        showSlider()
        val thumb = composeRule.onNodeWithContentDescription("停止引擎滑块")
        val initialLeft = thumb.fetchSemanticsNode().boundsInRoot.left
        composeRule.onNodeWithContentDescription("引擎停止滑轨").performTouchInput {
            down(Offset(26.dp.toPx(), centerY))
            moveTo(Offset(width - 2f, centerY), 400)
            cancel()
        }
        composeRule.waitForIdle()
        assertEquals(initialLeft, thumb.fetchSemanticsNode().boundsInRoot.left, .1f)
        composeRule.runOnIdle { assertEquals(0, stopCount) }
    }

    @Test
    fun draggingTrackInsteadOfThumbDoesNotStopEngine() {
        showSlider()
        composeRule.onNodeWithContentDescription("引擎停止滑轨").performTouchInput {
            swipe(Offset(width * .5f, centerY), Offset(width - 2f, centerY), 400)
        }
        composeRule.runOnIdle { assertEquals(0, stopCount) }
    }

    @Test
    fun stoppedHeaderShowsStartAndDisablesRecordingWithoutOldTitle() {
        var startCount = 0
        composeRule.setContent {
            MaterialTheme {
                EngineControlBar(
                    running = false,
                    recording = false,
                    elapsed = 0,
                    hasPermission = true,
                    onStart = { startCount++ },
                    onStop = { stopCount++ },
                    onRecordingChange = {}
                )
            }
        }
        composeRule.onNodeWithText("录音").assertIsNotEnabled()
        composeRule.onNodeWithText("LiveAudioProcess").assertDoesNotExist()
        composeRule.onNodeWithText("LIVE").assertDoesNotExist()
        composeRule.onNodeWithText("OFFLINE").assertDoesNotExist()
        composeRule.onNodeWithText("开始引擎").performClick()
        composeRule.runOnIdle {
            assertEquals(1, startCount)
            assertEquals(0, stopCount)
        }
    }

    @Test
    fun runningHeaderShowsRecordingTimerAndRecordingAction() {
        var recordingClicks = 0
        composeRule.setContent {
            MaterialTheme {
                EngineControlBar(
                    running = true,
                    recording = true,
                    elapsed = 65,
                    hasPermission = true,
                    onStart = {},
                    onStop = { stopCount++ },
                    onRecordingChange = { recordingClicks++ }
                )
            }
        }
        composeRule.onNodeWithText("01:05").assertExists()
        composeRule.onNodeWithText("停止录音").performClick()
        composeRule.runOnIdle {
            assertEquals(1, recordingClicks)
            assertEquals(0, stopCount)
        }
    }
}
