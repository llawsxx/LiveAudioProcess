package com.llawsxx.audioprocess

import android.os.Build

object NativeAudio {
    val available: Boolean = Build.VERSION.SDK_INT >= 26 && runCatching {
        System.loadLibrary("audioprocess")
    }.isSuccess

    external fun start(inputSampleRate: Int, outputSampleRate: Int, frames: Int, inputDeviceId: Int, outputDeviceId: Int, enableOutput: Boolean, inputChannels: Int, pair: Int, useNetworkInput: Boolean, usbFd: Int, usbInputHost: Boolean, usbOutputHost: Boolean, usbInputBitDepth: Int, usbOutputBitDepth: Int, usbInputBurstPackets: Int, usbOutputBurstPackets: Int): Boolean
    external fun stop()
    external fun stopForRouteChange()
    external fun update(flags: Int, values: FloatArray)
    external fun configureTone(enabled: Boolean, waveform: Int, music: Int, channels: Int, frequency: Float, frequency2: Float, durationSeconds: Float, clickIntervalMs: Float, level: Float)
    external fun levels(): FloatArray
    external fun waveform(): FloatArray
    external fun startRecordingFd(dryFd: Int, wetFd: Int): Boolean
    external fun stopRecording()
    external fun configureNetwork(role: Int, transport: Int, codec: Int, sampleRate: Int, bitrate: Int, host: String, port: Int, minBufferMs: Int, maxBufferMs: Int): Boolean
    external fun configureUsbOutputBuffer(maxBufferMs: Int)
    external fun configureUsbInputBuffer(maxBufferMs: Int)
    external fun setUsbVolume(percent: Int): Boolean
    external fun configureOutputBufferMaxMs(maxMs: Int)
    external fun configureInputBufferMaxMs(maxMs: Int)
    external fun clearNetwork()
    external fun networkErrorInfo(): IntArray
    external fun networkInputTimedOut(timeoutMs: Int): Boolean
    external fun networkOutputTimedOut(timeoutMs: Int): Boolean
    external fun networkOutputConnected(): Boolean
    external fun networkOutputConnectAttempts(): Int
    external fun routeInfo(): IntArray
    external fun usbHostFailed(): Boolean
    external fun inputInfo(): LongArray
    external fun outputInfo(): LongArray
    external fun usbStats(): LongArray
}
