package com.llawsxx.audioprocess

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.media.AudioDeviceInfo
import android.os.Build

data class AudioDeviceChoice(val key: String, val label: String)

const val SYSTEM_DEVICE_DEFAULT = "system:default"
const val SYSTEM_DEVICE_BUILTIN_INPUT = "system:builtin-input"
const val SYSTEM_DEVICE_BUILTIN_OUTPUT = "system:builtin-output"
const val USB_HOST_DEVICE_AUTO = "usb:auto"

fun systemAudioDeviceKey(device: AudioDeviceInfo): String =
    "audio:${device.type}:${device.address}:${device.productName}"

fun usbHostDeviceKey(device: UsbDevice): String =
    "usb:${device.vendorId}:${device.productId}:${device.deviceName}"

fun isUsbAudioDevice(device: UsbDevice): Boolean =
    (0 until device.interfaceCount).any {
        device.getInterface(it).interfaceClass == UsbConstants.USB_CLASS_AUDIO
    }

fun usbAudioDevices(manager: UsbManager): List<UsbDevice> =
    manager.deviceList.values.filter(::isUsbAudioDevice)
        .sortedWith(compareBy({ it.vendorId }, { it.productId }, { it.deviceName }))

fun findUsbAudioDevice(manager: UsbManager, key: String): UsbDevice? {
    val devices = usbAudioDevices(manager)
    if (key == USB_HOST_DEVICE_AUTO) return devices.firstOrNull()
    devices.firstOrNull { usbHostDeviceKey(it) == key }?.let { return it }
    val parts = key.split(':')
    if (parts.size >= 4 && parts[0] == "usb") {
        val vendor = parts[1].toIntOrNull()
        val product = parts[2].toIntOrNull()
        val sameModel = devices.filter { it.vendorId == vendor && it.productId == product }
        if (sameModel.size == 1) return sameModel.first()
    }
    return null
}

fun usbHostDeviceLabel(device: UsbDevice): String {
    val name = runCatching { device.productName?.takeIf(String::isNotBlank) }.getOrNull()
        ?: "USB Audio"
    return "$name · %04X:%04X · %s".format(device.vendorId, device.productId, device.deviceName.substringAfterLast('/'))
}

fun systemAudioDeviceLabel(device: AudioDeviceInfo): String {
    val type = when (device.type) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> "内置麦克风"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "内置扬声器"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "有线耳麦"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "有线耳机"
        AudioDeviceInfo.TYPE_LINE_ANALOG -> "模拟线路"
        AudioDeviceInfo.TYPE_LINE_DIGITAL -> "数字线路"
        AudioDeviceInfo.TYPE_USB_DEVICE -> "USB 声卡"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB 耳麦"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "蓝牙 A2DP"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "蓝牙 SCO"
        AudioDeviceInfo.TYPE_HDMI -> "HDMI"
        else -> if (Build.VERSION.SDK_INT >= 31 &&
            (device.type == AudioDeviceInfo.TYPE_BLE_HEADSET || device.type == AudioDeviceInfo.TYPE_BLE_SPEAKER)) "蓝牙 LE"
        else "音频设备"
    }
    val product = device.productName.toString().takeIf { it.isNotBlank() && it != type }
    return if (product == null) "$type · ID ${device.id}" else "$type · $product · ID ${device.id}"
}
