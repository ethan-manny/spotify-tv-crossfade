package dev.spotifytv.crossfade.patches.crossfade

import app.morphe.patcher.patch.resourcePatch
import dev.spotifytv.crossfade.patches.shared.Constants.COMPATIBILITY_SPOTIFY_TV
import org.w3c.dom.Element

/**
 * Internal: makes the installer extract native libraries so our injected libxfade.so
 * does not depend on the rebuilt APK's alignment or compression.
 */
val manifestPatch = resourcePatch {
    compatibleWith(COMPATIBILITY_SPOTIFY_TV)

    execute {
        document("AndroidManifest.xml").use { doc ->
            val application = doc.getElementsByTagName("application").item(0) as Element
            application.setAttribute("android:extractNativeLibs", "true")

            val activity = doc.createElement("activity")
            activity.setAttribute("android:name", "dev.spotifytv.crossfade.extension.CrossfadeSettingsActivity")
            activity.setAttribute("android:exported", "true")   // the adb intent (spec 6) needs it; the screen only edits our own setting
            activity.setAttribute("android:theme", "@android:style/Theme.DeviceDefault.NoActionBar")
            application.appendChild(activity)
        }
    }
}
