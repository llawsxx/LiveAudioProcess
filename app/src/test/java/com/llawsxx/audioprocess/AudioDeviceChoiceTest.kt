package com.llawsxx.audioprocess

import android.media.AudioDeviceInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

class AudioDeviceChoiceTest {
    @Test
    fun builtInMicrophonesWithSameModelAndAddressHaveDistinctKeys() {
        val firstKey = systemAudioDeviceKey(AudioDeviceInfo.TYPE_BUILTIN_MIC, "", "V2338A", 19)
        val secondKey = systemAudioDeviceKey(AudioDeviceInfo.TYPE_BUILTIN_MIC, "", "V2338A", 20)
        assertNotEquals(firstKey, secondKey)
        val choices = listOf(
            AudioDeviceChoice(firstKey, systemAudioDeviceLabel(AudioDeviceInfo.TYPE_BUILTIN_MIC, "V2338A", 19)),
            AudioDeviceChoice(secondKey, systemAudioDeviceLabel(AudioDeviceInfo.TYPE_BUILTIN_MIC, "V2338A", 20))
        ).distinctBy { it.key }
        assertEquals(2, choices.size)
        assertEquals("内置麦克风 · V2338A · ID 19", choices[0].label)
        assertEquals("内置麦克风 · V2338A · ID 20", choices[1].label)
    }

    @Test
    fun otherDeviceKeysKeepTheirExistingFormat() {
        assertEquals(
            "audio:22:usb-address:USB Audio",
            systemAudioDeviceKey(AudioDeviceInfo.TYPE_USB_HEADSET, "usb-address", "USB Audio", 23)
        )
        assertEquals(
            systemAudioDeviceKey(AudioDeviceInfo.TYPE_USB_HEADSET, "usb-address", "USB Audio", 23),
            systemAudioDeviceKey(AudioDeviceInfo.TYPE_USB_HEADSET, "usb-address", "USB Audio", 24)
        )
    }

    @Test
    fun builtInMicrophoneIncludesTypeModelAndId() {
        assertEquals("内置麦克风 · V2338A · ID 23", systemAudioDeviceLabel(AudioDeviceInfo.TYPE_BUILTIN_MIC, "V2338A", 23))
    }

    @Test
    fun fmTunerIncludesTypeModelAndId() {
        assertEquals("FM 调谐器 · V2339A · ID 24", systemAudioDeviceLabel(AudioDeviceInfo.TYPE_FM_TUNER, "V2339A", 24))
    }

    @Test
    fun inputDeviceTypesHaveSpecificLabels() {
        val labels = mapOf(
            AudioDeviceInfo.TYPE_FM to "FM 发射器",
            AudioDeviceInfo.TYPE_TV_TUNER to "电视调谐器",
            AudioDeviceInfo.TYPE_TELEPHONY to "电话音频",
            AudioDeviceInfo.TYPE_BUS to "音频总线",
            AudioDeviceInfo.TYPE_REMOTE_SUBMIX to "远程混音",
            AudioDeviceInfo.TYPE_USB_ACCESSORY to "USB 音频配件"
        )
        labels.forEach { (deviceType, label) ->
            assertEquals("$label · V2338A · ID 23", systemAudioDeviceLabel(deviceType, "V2338A", 23))
        }
    }

    @Test
    fun outputDeviceTypesHaveSpecificLabels() {
        val labels = mapOf(
            AudioDeviceInfo.TYPE_BUILTIN_EARPIECE to "内置听筒",
            AudioDeviceInfo.TYPE_BUILTIN_SPEAKER_SAFE to "内置扬声器（安全音量）",
            AudioDeviceInfo.TYPE_HDMI_ARC to "HDMI ARC",
            AudioDeviceInfo.TYPE_HDMI_EARC to "HDMI eARC",
            AudioDeviceInfo.TYPE_BLE_HEADSET to "蓝牙 LE 耳麦",
            AudioDeviceInfo.TYPE_BLE_SPEAKER to "蓝牙 LE 扬声器",
            AudioDeviceInfo.TYPE_DOCK_ANALOG to "模拟音频底座",
            AudioDeviceInfo.TYPE_MULTICHANNEL_GROUP to "多声道扬声器组"
        )
        labels.forEach { (deviceType, label) ->
            assertEquals("$label · V2338A · ID 23", systemAudioDeviceLabel(deviceType, "V2338A", 23))
        }
    }

    @Test
    fun missingOrDuplicateProductNameIsOmitted() {
        listOf(null, "", " ", "内置麦克风").forEach { productName ->
            assertEquals("内置麦克风 · ID 23", systemAudioDeviceLabel(AudioDeviceInfo.TYPE_BUILTIN_MIC, productName, 23))
        }
    }

    @Test
    fun unknownDeviceTypeKeepsDiagnosticInformation() {
        assertEquals("未知音频设备 · V2338A · ID 23", systemAudioDeviceLabel(AudioDeviceInfo.TYPE_UNKNOWN, "V2338A", 23))
        assertEquals("未知音频设备（类型 999） · V2338A · ID 23", systemAudioDeviceLabel(999, "V2338A", 23))
    }
}
