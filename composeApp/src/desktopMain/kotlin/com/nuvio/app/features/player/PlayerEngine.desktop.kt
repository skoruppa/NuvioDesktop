package com.nuvio.app.features.player

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.awt.SwingPanel
import androidx.compose.ui.graphics.Color
import com.nuvio.app.features.player.desktop.AwtNativePlayerHost
import com.nuvio.app.features.player.desktop.DesktopHostOs
import com.nuvio.app.features.player.desktop.DesktopPlayerLaunchShield
import com.nuvio.app.features.player.desktop.JavaFxPlayerHost
import com.nuvio.app.features.player.desktop.JavaFxPlayerSurface
import com.nuvio.app.features.player.desktop.LinuxPlayerHost
import com.nuvio.app.features.player.desktop.NativePlayerBridge
import com.nuvio.app.features.player.desktop.NativePlayerController
import com.nuvio.app.features.player.desktop.updateJavaFxControls
import kotlinx.coroutines.delay

@Composable
actual fun PlatformPlayerSurface(
    sourceUrl: String,
    sourceAudioUrl: String?,
    sourceHeaders: Map<String, String>,
    sourceResponseHeaders: Map<String, String>,
    externalSubtitles: List<com.nuvio.app.features.streams.StreamSubtitle>,
    streamType: String?,
    useYoutubeChunkedPlayback: Boolean,
    modifier: Modifier,
    playWhenReady: Boolean,
    resizeMode: PlayerResizeMode,
    initialPositionMs: Long,
    useNativeController: Boolean,
    playerControlsState: PlayerControlsState,
    onPlayerControlsAction: (PlayerControlsAction) -> Boolean,
    onPlayerControlsEvent: (String, Double) -> Boolean,
    onPlayerControlsScrubChange: (Long) -> Boolean,
    onPlayerControlsScrubFinished: (Long) -> Boolean,
    onControllerReady: (PlayerEngineController) -> Unit,
    onSnapshot: (PlayerPlaybackSnapshot) -> Unit,
    onError: (String?) -> Unit,
) {
    when (DesktopHostOs.current) {
        DesktopHostOs.LINUX -> {
            if (!DesktopHostOs.isWayland) {
                // Linux X11: AWT Canvas + SwingPanel — mpv يعرض عبر X11 WID داخل SwingPanel
                // WebKitGTK فوقه كـ overlay (GtkPlug) — نفس نموذج macOS/Windows
                LegacyAwtNativePlayerSurface(
                    sourceUrl = sourceUrl,
                    sourceHeaders = sourceHeaders,
                    modifier = modifier,
                    playWhenReady = playWhenReady,
                    resizeMode = resizeMode,
                    initialPositionMs = initialPositionMs,
                    playerControlsState = playerControlsState,
                    onPlayerControlsAction = onPlayerControlsAction,
                    onPlayerControlsEvent = onPlayerControlsEvent,
                    onPlayerControlsScrubChange = onPlayerControlsScrubChange,
                    onPlayerControlsScrubFinished = onPlayerControlsScrubFinished,
                    onControllerReady = onControllerReady,
                    onSnapshot = onSnapshot,
                    onError = onError,
                )
            } else {
                // Linux Wayland: AWT offscreen mpv + JavaFX WebView controls overlay
                WaylandJavaFxPlayerSurface(
                    sourceUrl = sourceUrl,
                    sourceHeaders = sourceHeaders,
                    modifier = modifier,
                    playWhenReady = playWhenReady,
                    resizeMode = resizeMode,
                    initialPositionMs = initialPositionMs,
                    playerControlsState = playerControlsState,
                    onPlayerControlsAction = onPlayerControlsAction,
                    onPlayerControlsEvent = onPlayerControlsEvent,
                    onPlayerControlsScrubChange = onPlayerControlsScrubChange,
                    onPlayerControlsScrubFinished = onPlayerControlsScrubFinished,
                    onControllerReady = onControllerReady,
                    onSnapshot = onSnapshot,
                    onError = onError,
                )
            }
        }
        DesktopHostOs.MACOS, DesktopHostOs.WINDOWS -> {
            // macOS / Windows: AWT Canvas + SwingPanel
            LegacyAwtNativePlayerSurface(
                sourceUrl = sourceUrl,
                sourceHeaders = sourceHeaders,
                modifier = modifier,
                playWhenReady = playWhenReady,
                resizeMode = resizeMode,
                initialPositionMs = initialPositionMs,
                playerControlsState = playerControlsState,
                onPlayerControlsAction = onPlayerControlsAction,
                onPlayerControlsEvent = onPlayerControlsEvent,
                onPlayerControlsScrubChange = onPlayerControlsScrubChange,
                onPlayerControlsScrubFinished = onPlayerControlsScrubFinished,
                onControllerReady = onControllerReady,
                onSnapshot = onSnapshot,
                onError = onError,
            )
        }
        else -> {
            DesktopStubPlayerSurface(
                modifier = modifier,
                onControllerReady = onControllerReady,
                onSnapshot = onSnapshot,
            )
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Linux: GTK standalone — نافذة واحدة = فيديو + controls
// Compose يعرض صندوق أسود placeholder فقط
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun LinuxGtkPlayerSurface(
    sourceUrl: String,
    sourceHeaders: Map<String, String>,
    modifier: Modifier,
    playWhenReady: Boolean,
    resizeMode: PlayerResizeMode,
    initialPositionMs: Long,
    playerControlsState: PlayerControlsState,
    onPlayerControlsAction: (PlayerControlsAction) -> Boolean,
    onPlayerControlsEvent: (String, Double) -> Boolean,
    onPlayerControlsScrubChange: (Long) -> Boolean,
    onPlayerControlsScrubFinished: (Long) -> Boolean,
    onControllerReady: (PlayerEngineController) -> Unit,
    onSnapshot: (PlayerPlaybackSnapshot) -> Unit,
    onError: (String?) -> Unit,
) {
    val host = remember { LinuxPlayerHost() }
    val controller = remember(host) { NativePlayerController(host) }
    val playbackHeaders = remember(sourceHeaders) { sanitizePlaybackHeaders(sourceHeaders) }

    val latestOnPlayerControlsAction = rememberUpdatedState(onPlayerControlsAction)
    val latestOnPlayerControlsEvent = rememberUpdatedState(onPlayerControlsEvent)
    val latestOnPlayerControlsScrubChange = rememberUpdatedState(onPlayerControlsScrubChange)
    val latestOnPlayerControlsScrubFinished = rememberUpdatedState(onPlayerControlsScrubFinished)
    val latestOnError = rememberUpdatedState(onError)

    LaunchedEffect(controller) {
        onControllerReady(controller)
    }

    LaunchedEffect(controller) {
        controller.setControlCallbacks(
            onAction = { action -> latestOnPlayerControlsAction.value(action) },
            onEvent = { type, value -> latestOnPlayerControlsEvent.value(type, value) },
            onScrubChange = { positionMs -> latestOnPlayerControlsScrubChange.value(positionMs) },
            onScrubFinished = { positionMs -> latestOnPlayerControlsScrubFinished.value(positionMs) },
        )
    }

    // على Linux لا ننتظر AWT peer — نشغّل مباشرة عبر GTK
    DisposableEffect(controller, sourceUrl, playbackHeaders) {
        controller.attach(
            sourceUrl = sourceUrl,
            sourceHeaders = playbackHeaders,
            playWhenReady = playWhenReady,
            initialPositionMs = initialPositionMs,
            onError = { message -> latestOnError.value(message) },
        )
        onDispose { controller.dispose() }
    }

    LaunchedEffect(controller, playWhenReady) {
        if (playWhenReady) controller.play() else controller.pause()
    }

    LaunchedEffect(controller, resizeMode) {
        controller.setResizeMode(resizeMode)
    }

    LaunchedEffect(controller, playerControlsState) {
        controller.updateControls(playerControlsState)
    }

    LaunchedEffect(controller) {
        while (true) {
            onSnapshot(controller.snapshot())
            delay(500L)
        }
    }

    // صندوق أسود — الفيديو في GTK window المستقلة
    Box(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black),
    )
}

// ─────────────────────────────────────────────────────────────────────────────
// Linux Wayland: AWT offscreen mpv + JavaFX WebView controls overlay
// واجهة واحدة فقط: controls.html عبر JavaFX WebView
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun WaylandJavaFxPlayerSurface(
    sourceUrl: String,
    sourceHeaders: Map<String, String>,
    modifier: Modifier,
    playWhenReady: Boolean,
    resizeMode: PlayerResizeMode,
    initialPositionMs: Long,
    playerControlsState: PlayerControlsState,
    onPlayerControlsAction: (PlayerControlsAction) -> Boolean,
    onPlayerControlsEvent: (String, Double) -> Boolean,
    onPlayerControlsScrubChange: (Long) -> Boolean,
    onPlayerControlsScrubFinished: (Long) -> Boolean,
    onControllerReady: (PlayerEngineController) -> Unit,
    onSnapshot: (PlayerPlaybackSnapshot) -> Unit,
    onError: (String?) -> Unit,
) {
    val host = remember { JavaFxPlayerHost() }
    val controller = remember(host) { NativePlayerController(host) }

    LaunchedEffect(sourceUrl) {
        DesktopPlayerLaunchShield.showForActiveWindow()
    }

    val playbackHeaders = remember(sourceHeaders) { sanitizePlaybackHeaders(sourceHeaders) }
    val latestOnPlayerControlsAction = rememberUpdatedState(onPlayerControlsAction)
    val latestOnPlayerControlsEvent = rememberUpdatedState(onPlayerControlsEvent)
    val latestOnPlayerControlsScrubChange = rememberUpdatedState(onPlayerControlsScrubChange)
    val latestOnPlayerControlsScrubFinished = rememberUpdatedState(onPlayerControlsScrubFinished)
    val latestOnError = rememberUpdatedState(onError)

    LaunchedEffect(controller) {
        onControllerReady(controller)
    }

    LaunchedEffect(controller) {
        controller.setControlCallbacks(
            onAction = { action -> latestOnPlayerControlsAction.value(action) },
            onEvent = { type, value -> latestOnPlayerControlsEvent.value(type, value) },
            onScrubChange = { positionMs -> latestOnPlayerControlsScrubChange.value(positionMs) },
            onScrubFinished = { positionMs -> latestOnPlayerControlsScrubFinished.value(positionMs) },
        )
    }

    LaunchedEffect(controller) {
        controller.setControlsUpdateCallback { json ->
            updateJavaFxControls(json)
        }
    }

    DisposableEffect(controller, sourceUrl, playbackHeaders) {
        onDispose {
            controller.dispose()
            DesktopPlayerLaunchShield.hide()
        }
    }

    LaunchedEffect(controller, sourceUrl, playbackHeaders) {
        controller.attach(
            sourceUrl = sourceUrl,
            sourceHeaders = playbackHeaders,
            playWhenReady = playWhenReady,
            initialPositionMs = initialPositionMs,
            onError = { message -> latestOnError.value(message) },
        )
    }

    LaunchedEffect(controller, playWhenReady) {
        if (playWhenReady) controller.play() else controller.pause()
    }

    LaunchedEffect(controller, resizeMode) {
        controller.setResizeMode(resizeMode)
    }

    LaunchedEffect(controller, playerControlsState) {
        controller.updateControls(playerControlsState)
    }

    LaunchedEffect(controller) {
        while (true) {
            onSnapshot(controller.snapshot())
            delay(500L)
        }
    }

    val controlsUrl = remember { NativePlayerBridge.controlsPageUrl }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black),
    ) {
        JavaFxPlayerSurface(
            controlsPageUrl = controlsUrl,
            host = host,
            onPlayerEvent = { type, value ->
                latestOnPlayerControlsEvent.value(type, value)
            },
            modifier = Modifier.fillMaxSize(),
        )
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// macOS / Windows: AWT Canvas + SwingPanel (المسار الأصلي)
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun LegacyAwtNativePlayerSurface(
    sourceUrl: String,
    sourceHeaders: Map<String, String>,
    modifier: Modifier,
    playWhenReady: Boolean,
    resizeMode: PlayerResizeMode,
    initialPositionMs: Long,
    playerControlsState: PlayerControlsState,
    onPlayerControlsAction: (PlayerControlsAction) -> Boolean,
    onPlayerControlsEvent: (String, Double) -> Boolean,
    onPlayerControlsScrubChange: (Long) -> Boolean,
    onPlayerControlsScrubFinished: (Long) -> Boolean,
    onControllerReady: (PlayerEngineController) -> Unit,
    onSnapshot: (PlayerPlaybackSnapshot) -> Unit,
    onError: (String?) -> Unit,
) {
    val host = remember { AwtNativePlayerHost() }
    val controller = remember(host) { NativePlayerController(host) }
    val attached = remember { mutableStateOf(false) }

    LaunchedEffect(sourceUrl) {
        DesktopPlayerLaunchShield.showForActiveWindow()
    }

    val playbackHeaders = remember(sourceHeaders) { sanitizePlaybackHeaders(sourceHeaders) }
    val latestOnPlayerControlsAction = rememberUpdatedState(onPlayerControlsAction)
    val latestOnPlayerControlsEvent = rememberUpdatedState(onPlayerControlsEvent)
    val latestOnPlayerControlsScrubChange = rememberUpdatedState(onPlayerControlsScrubChange)
    val latestOnPlayerControlsScrubFinished = rememberUpdatedState(onPlayerControlsScrubFinished)
    val latestOnError = rememberUpdatedState(onError)

    LaunchedEffect(controller) {
        onControllerReady(controller)
    }

    DisposableEffect(host) {
        host.onDisplayableChanged = { displayable ->
            if (!displayable) attached.value = false
        }
        host.onFirstPaint = {
            DesktopPlayerLaunchShield.hideAfter()
            if (!attached.value) {
                attached.value = true
                System.err.println("[NUVIO_SURFACE] onFirstPaint, calling attach()")
            }
        }
        onDispose {
            host.onDisplayableChanged = null
            host.onFirstPaint = null
            host.dispose()
            DesktopPlayerLaunchShield.hide()
        }
    }

    LaunchedEffect(controller) {
        controller.setControlCallbacks(
            onAction = { action -> latestOnPlayerControlsAction.value(action) },
            onEvent = { type, value -> latestOnPlayerControlsEvent.value(type, value) },
            onScrubChange = { positionMs -> latestOnPlayerControlsScrubChange.value(positionMs) },
            onScrubFinished = { positionMs -> latestOnPlayerControlsScrubFinished.value(positionMs) },
        )
    }

    DisposableEffect(controller, sourceUrl, playbackHeaders) {
        onDispose { controller.dispose() }
    }

    LaunchedEffect(controller, sourceUrl, playbackHeaders, attached.value) {
        if (!attached.value) return@LaunchedEffect
        delay(16L)
        System.err.println("[NUVIO_SURFACE] attached=true, calling attach()")
        controller.attach(
            sourceUrl = sourceUrl,
            sourceHeaders = playbackHeaders,
            playWhenReady = playWhenReady,
            initialPositionMs = initialPositionMs,
            onError = { message -> latestOnError.value(message) },
        )
    }

    LaunchedEffect(controller, playWhenReady) {
        if (playWhenReady) controller.play() else controller.pause()
    }

    LaunchedEffect(controller, resizeMode) {
        controller.setResizeMode(resizeMode)
    }

    LaunchedEffect(controller, playerControlsState) {
        controller.updateControls(playerControlsState)
    }

    LaunchedEffect(controller) {
        while (true) {
            onSnapshot(controller.snapshot())
            delay(500L)
        }
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black),
    ) {
        SwingPanel(
            factory = { host },
            modifier = Modifier.fillMaxSize(),
            background = Color.Black,
        )
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stub للأنظمة غير المدعومة
// ─────────────────────────────────────────────────────────────────────────────
@Composable
private fun DesktopStubPlayerSurface(
    modifier: Modifier,
    onControllerReady: (PlayerEngineController) -> Unit,
    onSnapshot: (PlayerPlaybackSnapshot) -> Unit,
) {
    val controller = remember { DesktopStubPlayerController() }

    LaunchedEffect(controller) {
        onControllerReady(controller)
        onSnapshot(PlayerPlaybackSnapshot(isLoading = false))
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = "Desktop in-app playback is not available yet.",
            color = Color.White,
            style = MaterialTheme.typography.bodyMedium,
        )
    }
}

private class DesktopStubPlayerController : PlayerEngineController {
    override fun play() = Unit
    override fun pause() = Unit
    override fun seekTo(positionMs: Long) = Unit
    override fun seekBy(offsetMs: Long) = Unit
    override fun retry() = Unit
    override fun setPlaybackSpeed(speed: Float) = Unit
    override fun getAudioTracks(): List<AudioTrack> = emptyList()
    override fun getSubtitleTracks(): List<SubtitleTrack> = emptyList()
    override fun selectAudioTrack(index: Int) = Unit
    override fun selectSubtitleTrack(index: Int) = Unit
    override fun setSubtitleUri(url: String) = Unit
    override fun clearExternalSubtitle() = Unit
    override fun clearExternalSubtitleAndSelect(trackIndex: Int) = Unit
}
