package com.nuvio.app.features.player.desktop

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.awt.SwingPanel
import androidx.compose.ui.graphics.Color
import javafx.animation.AnimationTimer
import javafx.application.Platform
import javafx.embed.swing.JFXPanel
import javafx.scene.Scene
import javafx.scene.canvas.Canvas
import javafx.scene.image.PixelFormat
import javafx.scene.layout.StackPane
import javafx.scene.web.WebEngine
import javafx.scene.web.WebView
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonPrimitive
import netscape.javascript.JSObject
import java.awt.KeyboardFocusManager
import java.awt.Point
import java.awt.Toolkit
import java.awt.image.BufferedImage
import java.util.concurrent.atomic.AtomicBoolean
import javax.swing.Timer

private val javafxInitialized = AtomicBoolean(false)

private fun ensureJavaFxInitialized() {
    if (javafxInitialized.compareAndSet(false, true)) {
        Platform.startup { }
    }
}

internal class JavaFxPlayerHost : PlayerHost {
    @Volatile
    override var nativeHandle: Long = 0L
    override var onMouseClick: (() -> Unit)? = null
    override var onCursorActivity: (() -> Unit)? = null
    private var controlsVisible = true
    private var cursorVisible = true
    private var cursorHideTimer: Timer? = null

    override fun setControlsVisible(visible: Boolean) {
        if (controlsVisible == visible) return
        controlsVisible = visible
        cancelCursorHideTimer()
        setCursorVisible(visible)
    }

    override fun noteCursorActivity() {
        if (controlsVisible) {
            cancelCursorHideTimer()
            setCursorVisible(true)
            return
        }
        setCursorVisible(true)
        restartCursorHideTimer()
    }

    override fun resetCursorVisibility() {
        controlsVisible = true
        cancelCursorHideTimer()
        setCursorVisible(true)
    }

    override fun dispose() {
        resetCursorVisibility()
    }

    private fun setCursorVisible(visible: Boolean) {
        if (cursorVisible == visible) return
        cursorVisible = visible
        val window = activeWindow ?: return
        window.cursor = if (visible) java.awt.Cursor.getDefaultCursor() else hiddenCursor
    }

    private fun restartCursorHideTimer() {
        cancelCursorHideTimer()
        cursorHideTimer = Timer(CursorIdleHideDelayMs) {
            if (!controlsVisible) {
                setCursorVisible(false)
            }
            cancelCursorHideTimer()
        }.apply {
            isRepeats = false
            start()
        }
    }

    private fun cancelCursorHideTimer() {
        cursorHideTimer?.stop()
        cursorHideTimer = null
    }

    private val activeWindow: java.awt.Window?
        get() = KeyboardFocusManager.getCurrentKeyboardFocusManager().activeWindow
            ?: java.awt.Window.getWindows().firstOrNull { it.isVisible && it.isActive }

    private companion object {
        const val CursorIdleHideDelayMs = 3_000

        val hiddenCursor: java.awt.Cursor by lazy {
            val image = BufferedImage(1, 1, BufferedImage.TYPE_INT_ARGB)
            Toolkit.getDefaultToolkit().createCustomCursor(image, Point(0, 0), "nuvio-hidden-cursor")
        }
    }
}

@Composable
internal fun JavaFxPlayerSurface(
    controlsPageUrl: String,
    host: JavaFxPlayerHost,
    onPlayerEvent: (type: String, value: Double) -> Unit,
    modifier: Modifier = Modifier,
) {
    val state = remember {
        JavaFxSurfaceState(controlsPageUrl, host, onPlayerEvent)
    }

    DisposableEffect(controlsPageUrl) {
        onDispose { state.dispose() }
    }

    SwingPanel(
        factory = {
            ensureJavaFxInitialized()
            state.createJfxPanel()
        },
        modifier = modifier,
        background = Color.Black,
    )
}

internal fun updateJavaFxControls(json: String) {
    JavaFxSurfaceState.latestInstance?.updateControls(json)
}

private class JavaFxSurfaceState(
    private val controlsPageUrl: String,
    private val host: JavaFxPlayerHost,
    private var onPlayerEvent: (String, Double) -> Unit,
) {
    private var jfxPanel: JFXPanel? = null
    private var canvas: Canvas? = null
    private var webView: WebView? = null
    private var engine: WebEngine? = null
    private var bridge: JavaFxPlayerBridge? = null
    private var pendingJson: String? = null
    private var renderTimer: AnimationTimer? = null

    private var lastWidth = 0
    private var lastHeight = 0
    private var pixelBuffer: IntArray? = null
    private var webViewReady = false
    private var onControlsStateUpdate: ((String) -> Unit)? = null

    fun updateCallback(callback: (String, Double) -> Unit) {
        onPlayerEvent = callback
    }

    fun setControlsUpdateCallback(callback: (String) -> Unit) {
        onControlsStateUpdate = callback
    }

    fun createJfxPanel(): JFXPanel {
        ensureJavaFxInitialized()

        val panel = JFXPanel()
        jfxPanel = panel

        Platform.runLater {
            val canvasInstance = Canvas()
            canvas = canvasInstance

            val webViewInstance = WebView().apply {
                isContextMenuEnabled = false
                engine.isJavaScriptEnabled = true
            }
            webView = webViewInstance
            engine = webViewInstance.engine

            bridge = JavaFxPlayerBridge { type, value ->
                onPlayerEvent(type, value)
            }

            val controlsPane = StackPane(webViewInstance)
            controlsPane.setPickOnBounds(false)

            val root = StackPane(canvasInstance, controlsPane)

            val scene = Scene(root, javafx.scene.paint.Color.BLACK)
            panel.scene = scene

            engine!!.load(controlsPageUrl)

            engine!!.loadWorker.stateProperty().addListener { _, _, newState ->
                if (newState == javafx.concurrent.Worker.State.SUCCEEDED) {
                    webViewReady = true
                    onWebViewReady()
                }
            }

            root.widthProperty().addListener { _, _, newVal ->
                val w = newVal.toInt().coerceAtLeast(1)
                if (w != lastWidth) {
                    canvasInstance.width = w.toDouble()
                    lastWidth = w
                }
            }
            root.heightProperty().addListener { _, _, newVal ->
                val h = newVal.toInt().coerceAtLeast(1)
                if (h != lastHeight) {
                    canvasInstance.height = h.toDouble()
                    lastHeight = h
                }
            }

            startRenderTimer()

            latestInstance = this
        }

        return panel
    }

    private fun startRenderTimer() {
        stopRenderTimer()
        renderTimer = object : AnimationTimer() {
            override fun handle(now: Long) {
                renderCurrentFrame()
            }
        }.apply { start() }
    }

    private fun stopRenderTimer() {
        renderTimer?.stop()
        renderTimer = null
    }

    private fun scheduleFrame() {
        val currentCanvas = canvas ?: return
        val root = currentCanvas.parent as? StackPane ?: return
        val w = root.width.toInt().coerceAtLeast(1)
        val h = root.height.toInt().coerceAtLeast(1)
        if (w != lastWidth || h != lastHeight) {
            currentCanvas.width = w.toDouble()
            currentCanvas.height = h.toDouble()
            lastWidth = w
            lastHeight = h
        }
    }

    private fun renderCurrentFrame() {
        val handle = host.nativeHandle
        if (handle == 0L) return
        val currentCanvas = canvas ?: return
        val sceneObj = currentCanvas.scene ?: return
        val sceneW = sceneObj.width.toInt().coerceAtLeast(1)
        val sceneH = sceneObj.height.toInt().coerceAtLeast(1)
        if (sceneW != lastWidth || sceneH != lastHeight) {
            System.err.println("[JAVAFX] Canvas resize: ${lastWidth}x${lastHeight} -> ${sceneW}x${sceneH}")
            currentCanvas.width = sceneW.toDouble()
            currentCanvas.height = sceneH.toDouble()
            lastWidth = sceneW
            lastHeight = sceneH
        }

        val count = sceneW * sceneH
        val pix = pixelBuffer?.takeIf { it.size >= count }
            ?: IntArray(count).also { pixelBuffer = it }

        if (!NativePlayerBridge.renderFrame(handle, pix, sceneW, sceneH)) return

        try {
            val gc = currentCanvas.graphicsContext2D
            val pixelWriter = gc.pixelWriter
            pixelWriter.setPixels(0, 0, sceneW, sceneH,
                PixelFormat.getIntArgbPreInstance(), pix, 0, sceneW)
        } catch (_: Exception) {
        }
    }

    fun onWebViewReady() {
        System.err.println("[JAVAFX] WebView loaded, injecting bridge")
        val currentEngine = engine ?: return
        val b = bridge ?: return
        Platform.runLater {
            try {
                val window = currentEngine.executeScript("window") as? JSObject
                window?.setMember("javaBridge", b)
                System.err.println("[JAVAFX] javaBridge injected successfully")
            } catch (e: Exception) {
                System.err.println("[JAVAFX] Failed to inject javaBridge: ${e.message}")
            }
            pendingJson?.let { json ->
                pendingJson = null
                System.err.println("[JAVAFX] Sending pending controls JSON")
                updateControls(json)
            }
            onControlsStateUpdate?.let { it("") }
        }
    }

    fun updateControls(json: String) {
        val currentEngine = engine ?: run {
            pendingJson = json
            return
        }
        Platform.runLater {
            try {
                val escaped = json
                    .replace("\\", "\\\\")
                    .replace("'", "\\'")
                    .replace("\n", "\\n")
                    .replace("\r", "\\r")
                currentEngine.executeScript(
                    "(function(){if(!window.playerControls)return;" +
                        "window.playerControls(JSON.parse('$escaped'));})()"
                )
            } catch (_: Exception) {
            }
        }
    }

    fun dispose() {
        stopRenderTimer()
        latestInstance = null
        val panel = jfxPanel
        val webV = webView
        jfxPanel = null
        webView = null
        engine = null
        bridge = null
        pendingJson = null
        canvas = null
        pixelBuffer = null

        if (panel != null) {
            Platform.runLater {
                try {
                    webV?.engine?.load(null)
                    panel.scene = null
                } catch (_: Exception) {
                }
            }
        }
    }

    companion object {
        var latestInstance: JavaFxSurfaceState? = null
            private set
    }
}

private class JavaFxPlayerBridge(
    private val onEvent: (type: String, value: Double) -> Unit,
) {
    private val json = Json { ignoreUnknownKeys = true }

    @Suppress("unused")
    fun postMessage(jsonStr: String) {
        try {
            val obj = json.decodeFromString<JsonObject>(jsonStr.trim())
            val type = obj["type"]?.jsonPrimitive?.content ?: return
            val value = obj["value"]?.jsonPrimitive?.content?.toDoubleOrNull() ?: 0.0
            System.err.println("[JAVAFX] bridge event: type=$type value=$value")
            onEvent(type, value)
        } catch (e: Exception) {
            System.err.println("[JAVAFX] bridge parse error: ${e.message}")
        }
    }
}
