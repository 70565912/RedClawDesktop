package com.redclaw.controllerspike

import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicLong

/**
 * Spike decoder: async MediaCodec rendering straight to a Surface.
 *
 * Latest-frame semantics: if no codec input buffer is free when a frame
 * arrives, the frame is dropped and every following frame is dropped until
 * the next keyframe (the reference chain is broken), and [onKeyframeNeeded]
 * asks the Host for an IDR. No frame queue is kept on the Kotlin side.
 */
class MediaCodecVideoSink(
    private val surface: Surface,
    private val onKeyframeNeeded: (String) -> Unit,
) {
    private val freeInputs = ArrayBlockingQueue<Int>(32)
    private var codec: MediaCodec? = null
    private var configuredMime: String? = null
    private var configuredWidth = 0
    private var configuredHeight = 0
    private var awaitingKeyframe = true

    val decodedFrames = AtomicLong()
    val droppedFrames = AtomicLong()
    val queuedFrames = AtomicLong()

    @Synchronized
    fun submit(codecId: Int, width: Int, height: Int, keyframe: Boolean, timestampMs: Long, data: ByteBuffer) {
        val mime = when (codecId) {
            1 -> MediaFormat.MIMETYPE_VIDEO_AVC
            2 -> MediaFormat.MIMETYPE_VIDEO_HEVC
            else -> {
                droppedFrames.incrementAndGet()
                return
            }
        }
        if (keyframe && (mime != configuredMime || width != configuredWidth || height != configuredHeight)) {
            reconfigure(mime, width, height)
        }
        val active = codec
        if (active == null || (awaitingKeyframe && !keyframe)) {
            droppedFrames.incrementAndGet()
            if (active == null) onKeyframeNeeded("decoder_not_configured")
            return
        }
        val index = freeInputs.poll()
        if (index == null) {
            droppedFrames.incrementAndGet()
            awaitingKeyframe = true
            onKeyframeNeeded("decoder_input_full")
            return
        }
        try {
            val input = active.getInputBuffer(index) ?: return
            input.clear()
            if (input.capacity() < data.remaining()) {
                active.queueInputBuffer(index, 0, 0, 0, 0)
                droppedFrames.incrementAndGet()
                awaitingKeyframe = true
                onKeyframeNeeded("decoder_input_too_small")
                return
            }
            val size = data.remaining()
            input.put(data)
            active.queueInputBuffer(
                index, 0, size, timestampMs * 1000L,
                if (keyframe) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0,
            )
            awaitingKeyframe = false
            queuedFrames.incrementAndGet()
        } catch (error: IllegalStateException) {
            Log.w(TAG, "queueInputBuffer failed", error)
            awaitingKeyframe = true
            onKeyframeNeeded("decoder_queue_failed")
        }
    }

    @Synchronized
    fun release() {
        codec?.let {
            try {
                it.stop()
            } catch (_: IllegalStateException) {
            }
            it.release()
        }
        codec = null
        configuredMime = null
        freeInputs.clear()
        awaitingKeyframe = true
    }

    private fun reconfigure(mime: String, width: Int, height: Int) {
        release()
        val format = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
        }
        val created = MediaCodec.createDecoderByType(mime)
        created.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(mc: MediaCodec, index: Int) {
                freeInputs.offer(index)
            }

            override fun onOutputBufferAvailable(mc: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                try {
                    mc.releaseOutputBuffer(index, info.size > 0)
                    if (info.size > 0) decodedFrames.incrementAndGet()
                } catch (_: IllegalStateException) {
                }
            }

            override fun onError(mc: MediaCodec, e: MediaCodec.CodecException) {
                Log.w(TAG, "decoder error recoverable=${e.isRecoverable}", e)
                onKeyframeNeeded("decoder_error")
            }

            override fun onOutputFormatChanged(mc: MediaCodec, format: MediaFormat) {
                Log.i(TAG, "decoder output format $format")
            }
        })
        created.configure(format, surface, null, 0)
        created.start()
        codec = created
        configuredMime = mime
        configuredWidth = width
        configuredHeight = height
        awaitingKeyframe = true
        Log.i(TAG, "decoder configured mime=$mime size=${width}x$height name=${created.name}")
    }

    private companion object {
        const val TAG = "RedClawDecoder"
    }
}
