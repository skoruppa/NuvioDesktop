package com.nuvio.app.features.player.desktop

import java.awt.Component
import java.lang.reflect.Field
import java.lang.reflect.Method

internal object AwtNativeViewResolver {
    fun resolveNativeViewPointer(component: Component): Long =
        when (DesktopHostOs.current) {
            DesktopHostOs.MACOS -> MacosAwtViewResolver.resolveNativeViewPointer(component)
            DesktopHostOs.WINDOWS -> WindowsAwtViewResolver.resolveNativeViewPointer(component)
            DesktopHostOs.LINUX -> LinuxAwtViewResolver.resolveNativeViewPointer(component)
            else -> error("Native desktop playback is not implemented for ${DesktopHostOs.current}.")
        }
}

private object MacosAwtViewResolver {
    private val componentPeerField: Field by lazy {
        Component::class.java.getDeclaredField("peer").apply { isAccessible = true }
    }

    fun resolveNativeViewPointer(component: Component): Long {
        val peer = componentPeerField.get(component)
            ?: error("AWT component peer is not ready for native playback.")

        val platformWindow = invokeObject(peer, "getPlatformWindow")
        val contentView = invokeObject(platformWindow, "getContentView")
        val pointer = invokeLong(contentView, "getAWTView")
        if (pointer == 0L) {
            error("macOS AWT view pointer was zero.")
        }
        return pointer
    }

    private fun findMethod(type: Class<*>, name: String): Method {
        var current: Class<*>? = type
        while (current != null) {
            runCatching {
                return current.getDeclaredMethod(name).apply { isAccessible = true }
            }
            current = current.superclass
        }
        error("Method $name was not found on ${type.name}.")
    }

    private fun invokeObject(target: Any, methodName: String): Any =
        findMethod(target.javaClass, methodName).invoke(target)
            ?: error("$methodName returned null.")

    private fun invokeLong(target: Any, methodName: String): Long =
        (findMethod(target.javaClass, methodName).invoke(target) as Number).toLong()
}

private object WindowsAwtViewResolver {
    private val componentPeerField: Field by lazy {
        Component::class.java.getDeclaredField("peer").apply { isAccessible = true }
    }

    fun resolveNativeViewPointer(component: Component): Long {
        val peer = componentPeerField.get(component)
            ?: error("AWT component peer is not ready for native playback.")

        val pointer = invokeLong(peer, "getHWnd")
        if (pointer == 0L) {
            error("Windows AWT HWND pointer was zero.")
        }
        return pointer
    }

    private fun findMethod(type: Class<*>, name: String): Method {
        var current: Class<*>? = type
        while (current != null) {
            runCatching {
                return current.getDeclaredMethod(name).apply { isAccessible = true }
            }
            current = current.superclass
        }
        error("Method $name was not found on ${type.name}.")
    }

    private fun invokeLong(target: Any, methodName: String): Long =
        (findMethod(target.javaClass, methodName).invoke(target) as Number).toLong()
}

private object LinuxAwtViewResolver {
    private val componentPeerField: Field by lazy {
        Component::class.java.getDeclaredField("peer").apply { isAccessible = true }
    }

    fun resolveNativeViewPointer(component: Component): Long {
        val peer = componentPeerField.get(component)
            ?: error("AWT component peer is not ready for native playback.")

        // XCanvasPeer extends ... extends XBaseWindow which has getWindow() returning X11 Window ID
        val pointer = invokeLongDeep(peer, "getWindow")
        if (pointer == 0L) {
            // Fallback: try getContentWindow() which also exists on XBaseWindow
            val contentWindow = invokeLongDeep(peer, "getContentWindow")
            if (contentWindow != 0L) return contentWindow
            error("Linux X11 AWT window pointer was zero.")
        }
        return pointer
    }

    private fun findMethodDeep(type: Class<*>, name: String): Method {
        var current: Class<*>? = type
        while (current != null) {
            for (m in current.declaredMethods) {
                if (m.name == name && m.parameterCount == 0) {
                    m.isAccessible = true
                    return m
                }
            }
            current = current.superclass
        }
        error("Method $name was not found on ${type.name}.")
    }

    private fun invokeLongDeep(target: Any, methodName: String): Long =
        (findMethodDeep(target.javaClass, methodName).invoke(target) as Number).toLong()
}
