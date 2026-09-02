package com.llawsxx.audioprocess

import android.os.Build

object NativeAudio {
    val available: Boolean = Build.VERSION.SDK_INT >= 26 && runCatching {
        System.loadLibrary("pulseforge_audio")
    }.isSuccess

    external fun start(sampleRate: Int, frames: Int, inputDeviceId: Int, outputDeviceId: Int, inputChannels: Int, pair: Int, useNetworkInput: Boolean, usbFd: Int, usbInputHost: Boolean, usbOutputHost: Boolean, usbBitDepth: Int, usbInputBurstPackets: Int, usbOutputBurstPackets: Int): Boolean
    external fun stop()
    external fun update(flags: Int, values: FloatArray)
    external fun levels(): FloatArray
    external fun startRecordingFd(dryFd: Int, wetFd: Int): Boolean
    external fun stopRecording()
    external fun configureNetwork(role: Int, codec: Int, host: String, port: Int, minBufferMs: Int, maxBufferMs: Int): Boolean
    external fun configureUsbOutputBuffer(minBufferMs: Int, maxBufferMs: Int)
    external fun configureOutputBufferMaxMs(maxMs: Int)
    external fun configureInputBufferMaxMs(maxMs: Int)
    external fun clearNetwork()
    external fun networkInputTimedOut(timeoutMs: Int): Boolean
    external fun routeInfo(): IntArray
    external fun inputInfo(): LongArray
    external fun outputInfo(): LongArray
    external fun usbStats(): LongArray
}
