package com.nuvio.app.features.player.desktop

/**
 * Common contract between [NativePlayerHost] (AWT Canvas, used on macOS/Windows/X11)
 * and [WaylandPlayerHost] (Compose-only, used on Linux Wayland).
 * Allows [NativePlayerController] to drive both hosts without duplication.
 */
internal interface PlayerHost {
    var nativeHandle: Long

    var onMouseClick: (() -> Unit)?
    var onDoubleClick: (() -> Unit)?
    var onCursorActivity: (() -> Unit)?

    val isDisplayable: Boolean get() = false

    fun setControlsVisible(visible: Boolean)
    fun noteCursorActivity()
    fun resetCursorVisibility()
    fun requestFocusInWindow(): Boolean = false
    fun dispose() {}
}
