package com.redclaw.controllerspike

import java.nio.ByteBuffer

/** Callbacks arrive on native threads; marshal UI work to the main thread. */
interface ControllerSessionListener {
    fun onPhase(phase: String, detail: String)

    /**
     * One complete encoded access unit (Annex-B). [data] aliases native memory
     * and is only valid during this call: copy it into a decoder input buffer
     * synchronously and never retain the reference.
     * codec: 1 = H.264, 2 = HEVC.
     */
    fun onEncodedFrame(
        codec: Int,
        width: Int,
        height: Int,
        keyframe: Boolean,
        timestampMs: Long,
        frameId: Long,
        data: ByteBuffer,
    )

    fun onLog(line: String)
}

/** Thin JNI surface over redclaw::controller_core::ControllerSession (S0.1 spike). */
object NativeControllerCore {
    init {
        System.loadLibrary("redclaw_controller_jni")
    }

    @JvmStatic external fun apiVersion(): Int

    @JvmStatic external fun create(listener: ControllerSessionListener): Long

    /**
     * Returns null on success or a redacted error. [passwordUtf8] is copied
     * natively; the caller should zero its array afterwards.
     */
    @JvmStatic external fun start(
        handle: Long,
        machineCode: String,
        passwordUtf8: ByteArray,
        mailboxDirectory: String,
        iceServers: Array<String>,
        dhtBootstrapNodes: Array<String>,
        iceUdpPort: Int,
        viewportWidth: Int,
        viewportHeight: Int,
    ): String?

    @JvmStatic external fun stop(handle: Long)

    @JvmStatic external fun destroy(handle: Long)

    @JvmStatic external fun setViewport(handle: Long, width: Int, height: Int): Boolean

    @JvmStatic external fun requestKeyframe(handle: Long, reason: String): Boolean

    @JvmStatic external fun stats(handle: Long): String
}
