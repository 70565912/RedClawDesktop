package com.redclaw.controllerspike

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.nio.ByteBuffer

/**
 * S0.1 spike shell. Deliberately ugly: machine code + password form, a
 * connect button, a status line and a SurfaceView fed by MediaCodec.
 */
class MainActivity : Activity(), ControllerSessionListener {
    private val mainHandler = Handler(Looper.getMainLooper())
    private lateinit var machineCode: EditText
    private lateinit var password: EditText
    private lateinit var iceServers: EditText
    private lateinit var iceUdpPort: EditText
    private lateinit var connect: Button
    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var surfaceView: SurfaceView

    private var handle = 0L
    @Volatile private var sink: MediaCodecVideoSink? = null
    private var touchSamples = 0L
    private var lastTouch = 0 to 0

    private fun normalize(position: Float, extent: Int): Int =
        (position / extent.coerceAtLeast(1) * 65535f).toInt().coerceIn(0, 65535)

    private val statsTicker = object : Runnable {
        override fun run() {
            if (handle != 0L) {
                val decoder = sink
                status.text = NativeControllerCore.stats(handle) +
                    (decoder?.let { " queued=${it.queuedFrames.get()} decoded=${it.decodedFrames.get()} decoder_dropped=${it.droppedFrames.get()}" } ?: "") +
                    " touch_samples=$touchSamples last_touch=${lastTouch.first},${lastTouch.second}"
            }
            mainHandler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        machineCode = EditText(this).apply {
            hint = "Host machine code"
            setText("RC7TST01")
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
        }
        password = EditText(this).apply {
            hint = "Connection password"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        iceServers = EditText(this).apply {
            hint = "ICE servers (comma separated)"
            setText("stun:stun.l.google.com:19302,stun:stun.cloudflare.com:3478")
        }
        iceUdpPort = EditText(this).apply {
            hint = "Local ICE UDP port"
            setText("55000")
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        connect = Button(this).apply {
            text = "Connect"
            setOnClickListener { toggleSession() }
        }
        status = TextView(this).apply { text = "native api=${NativeControllerCore.apiVersion()} idle" }
        log = TextView(this).apply { maxLines = 6 }
        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                sink = MediaCodecVideoSink(holder.surface) { reason ->
                    if (handle != 0L) NativeControllerCore.requestKeyframe(handle, reason)
                }
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
                if (handle != 0L) NativeControllerCore.setViewport(handle, width, height)
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                sink?.release()
                sink = null
            }
        })
        // S0.4 input path: normalized 0..65535 coordinates are what
        // RemoteInputEventV1 carries. Not sent yet (no input lease in S0.1).
        surfaceView.setOnTouchListener { view, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN || event.actionMasked == MotionEvent.ACTION_MOVE) {
                touchSamples++
                lastTouch = normalize(event.x, view.width) to normalize(event.y, view.height)
            }
            true
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(machineCode)
            addView(password)
            addView(iceServers)
            addView(iceUdpPort)
            addView(connect)
            addView(status)
            addView(log)
            addView(surfaceView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        setContentView(root)
        mainHandler.post(statsTicker)
    }

    override fun onDestroy() {
        mainHandler.removeCallbacks(statsTicker)
        closeSession()
        super.onDestroy()
    }

    private fun toggleSession() {
        if (handle != 0L) {
            closeSession()
            connect.text = "Connect"
            return
        }
        val secret = password.text.toString().toByteArray(Charsets.UTF_8)
        password.text.clear()
        handle = NativeControllerCore.create(this)
        val error = NativeControllerCore.start(
            handle,
            machineCode.text.toString().trim().uppercase(),
            secret,
            File(cacheDir, "dht-mailbox").absolutePath,
            iceServers.text.toString().split(',').map { it.trim() }.filter { it.isNotEmpty() }.toTypedArray(),
            emptyArray(),
            iceUdpPort.text.toString().toIntOrNull() ?: 55000,
            surfaceView.width,
            surfaceView.height,
        )
        secret.fill(0)
        if (error != null) {
            status.text = "start failed: $error"
            NativeControllerCore.destroy(handle)
            handle = 0L
            return
        }
        connect.text = "Disconnect"
    }

    private fun closeSession() {
        if (handle == 0L) return
        val closing = handle
        handle = 0L
        NativeControllerCore.destroy(closing)
        sink?.release()
    }

    override fun onPhase(phase: String, detail: String) {
        mainHandler.post { appendLog("phase=$phase $detail") }
    }

    override fun onEncodedFrame(
        codec: Int,
        width: Int,
        height: Int,
        keyframe: Boolean,
        timestampMs: Long,
        frameId: Long,
        data: ByteBuffer,
    ) {
        sink?.submit(codec, width, height, keyframe, timestampMs, data)
    }

    override fun onLog(line: String) {
        mainHandler.post { appendLog(line) }
    }

    private fun appendLog(line: String) {
        val lines = (log.text.toString().lines() + line).takeLast(6)
        log.text = lines.joinToString("\n")
    }
}
