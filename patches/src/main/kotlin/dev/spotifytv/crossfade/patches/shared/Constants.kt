package dev.spotifytv.crossfade.patches.shared

import app.morphe.patcher.patch.ApkFileType
import app.morphe.patcher.patch.AppTarget
import app.morphe.patcher.patch.Compatibility

object Constants {
    /** Spotify for TV (Fire TV / Android TV). Only 1.134.2 has been verified. */
    val COMPATIBILITY_SPOTIFY_TV = Compatibility(
        name = "Spotify (TV)",
        packageName = "com.spotify.tv.android",
        apkFileType = ApkFileType.APK,
        appIconColor = 0x1DB954,
        targets = listOf(
            AppTarget(version = "1.134.2"),
        ),
    )

    const val EXTENSION_PACKAGE = "Ldev/spotifytv/crossfade/extension/"
    const val HOOKS_CLASS = EXTENSION_PACKAGE + "CrossfadeHooks;"
}
