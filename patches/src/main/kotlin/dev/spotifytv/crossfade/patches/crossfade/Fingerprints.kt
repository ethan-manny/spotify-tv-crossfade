package dev.spotifytv.crossfade.patches.crossfade

import app.morphe.patcher.Fingerprint
import app.morphe.patcher.fingerprint
import com.android.tools.smali.dexlib2.AccessFlags

/** SpotifyTVApplication.onCreate(): unobfuscated class, instance method, no parameters. */
object ApplicationOnCreateFingerprint : Fingerprint(
    definingClass = "Lcom/spotify/tv/android/SpotifyTVApplication;",
    name = "onCreate",
    parameters = listOf(),
    returnType = "V",
)

private const val ROUTER_CLASS = "Lcom/spotify/tv/android/bindings/tvbridge/TVBridgeCallbacksRouter;"
private const val ACTIVITY_CLASS = "Lcom/spotify/tv/android/SpotifyTVActivity;"

/** The static lambda that parses the eSDK's metadata JSON (its first String parameter). Name is compiler-generated, so match by shape and strings. */
internal val metadataLambdaFingerprint = fingerprint {
    accessFlags(AccessFlags.PRIVATE, AccessFlags.STATIC, AccessFlags.FINAL)
    parameters(ROUTER_CLASS, "Ljava/lang/String;", "I", "Ljava/lang/String;")
    strings("playback_id", "duration_ms", "is_ad_playing", "track_uri", "album_cover_url")
    custom { _, classDef -> classDef.type == ROUTER_CLASS }
}

/** SpotifyTVActivity.dispatchKeyEvent(KeyEvent): forwards remote keys to the web client. */
internal val dispatchKeyEventFingerprint = fingerprint {
    accessFlags(AccessFlags.PUBLIC, AccessFlags.FINAL)
    returns("Z")
    parameters("Landroid/view/KeyEvent;")
    strings("isConsumed", "originalKeyCode")
    custom { method, classDef -> classDef.type == ACTIVITY_CLASS && method.name == "dispatchKeyEvent" }
}
