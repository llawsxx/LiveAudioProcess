package com.llawsxx.audioprocess

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.media.AudioDeviceInfo

data class AudioDeviceChoice(val key: String, val label: String)

const val SYSTEM_DEVICE_DEFAULT = "system:default"
const val SYSTEM_DEVICE_BUILTIN_INPUT = "system:builtin-input"
const val SYSTEM_DEVICE_BUILTIN_OUTPUT = "system:builtin-output"
const val USB_HOST_DEVICE_AUTO = "usb:auto"

fun systemAudioDeviceKey(device: AudioDeviceInfo): String =
    systemAudioDeviceKey(device.type, device.address, device.productName.toString(), device.id)

internal fun systemAudioDeviceKey(deviceType: Int, address: String, productName: String, deviceId: Int): String {
    val baseKey = "audio:$deviceType:$address:$productName"
    return if (deviceType == AudioDeviceInfo.TYPE_BUILTIN_MIC) "$baseKey:id:$deviceId" else baseKey
}

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

fun systemAudioDeviceLabel(device: AudioDeviceInfo): String =
    systemAudioDeviceLabel(device.type, device.productName?.toString(), device.id)

internal fun systemAudioDeviceLabel(deviceType: Int, productName: String?, deviceId: Int): String {
    val type = when (deviceType) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> "内置麦克风"
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "内置听筒"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "内置扬声器"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER_SAFE -> "内置扬声器（安全音量）"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "有线耳麦"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "有线耳机"
        AudioDeviceInfo.TYPE_LINE_ANALOG -> "模拟线路"
        AudioDeviceInfo.TYPE_LINE_DIGITAL -> "数字线路"
        AudioDeviceInfo.TYPE_USB_DEVICE -> "USB 声卡"
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB 音频配件"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB 耳麦"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "蓝牙 A2DP"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "蓝牙 SCO"
        AudioDeviceInfo.TYPE_BLE_HEADSET -> "蓝牙 LE 耳麦"
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> "蓝牙 LE 扬声器"
        AudioDeviceInfo.TYPE_BLE_BROADCAST -> "蓝牙 LE 广播"
        AudioDeviceInfo.TYPE_BLE_HEARING_AID -> "蓝牙 LE 助听器"
        AudioDeviceInfo.TYPE_BLE_CENTRAL -> "蓝牙音频中心设备"
        AudioDeviceInfo.TYPE_BLE_CENTRAL_BROADCAST -> "蓝牙 LE 广播中心设备"
        AudioDeviceInfo.TYPE_HEARING_AID -> "助听器"
        AudioDeviceInfo.TYPE_HDMI -> "HDMI"
        AudioDeviceInfo.TYPE_HDMI_ARC -> "HDMI ARC"
        AudioDeviceInfo.TYPE_HDMI_EARC -> "HDMI eARC"
        AudioDeviceInfo.TYPE_FM -> "FM 发射器"
        AudioDeviceInfo.TYPE_FM_TUNER -> "FM 调谐器"
        AudioDeviceInfo.TYPE_TV_TUNER -> "电视调谐器"
        AudioDeviceInfo.TYPE_TELEPHONY -> "电话音频"
        AudioDeviceInfo.TYPE_AUX_LINE -> "AUX 线路"
        AudioDeviceInfo.TYPE_DOCK -> "音频底座"
        AudioDeviceInfo.TYPE_DOCK_ANALOG -> "模拟音频底座"
        AudioDeviceInfo.TYPE_IP -> "IP 音频设备"
        AudioDeviceInfo.TYPE_BUS -> "音频总线"
        AudioDeviceInfo.TYPE_REMOTE_SUBMIX -> "远程混音"
        AudioDeviceInfo.TYPE_MULTICHANNEL_GROUP -> "多声道扬声器组"
        AudioDeviceInfo.TYPE_UNKNOWN -> "未知音频设备"
        else -> "未知音频设备（类型 $deviceType）"
    }
    val product = productName?.takeIf { it.isNotBlank() && it != type }
    return if (product == null) "$type · ID $deviceId" else "$type · $product · ID $deviceId"
}
