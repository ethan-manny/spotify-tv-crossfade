package dev.spotifytv.crossfade.patches.drm

import app.morphe.patcher.Fingerprint
import app.morphe.patcher.extensions.InstructionExtensions.addInstructions
import app.morphe.patcher.patch.bytecodePatch
import dev.spotifytv.crossfade.patches.shared.Constants.COMPATIBILITY_SPOTIFY_TV

private const val KIWI = "Lcom/amazon/android/Kiwi;"
private const val ACTIVITY = "Landroid/app/Activity;"
private const val SERVICE = "Landroid/app/Service;"

private fun kiwi(name: String, parameters: List<String>, returnType: String) =
    Fingerprint(definingClass = KIWI, name = name, parameters = parameters, returnType = returnType)

@Suppress("unused")
val disableAmazonDrmPatch = bytecodePatch(
    name = "Disable Amazon Appstore DRM",
    description = "Turns the Amazon Appstore (Kiwi) DRM lifecycle checks into no-ops so a re-signed APK runs.",
) {
    compatibleWith(COMPATIBILITY_SPOTIFY_TV)

    execute {
        listOf(
            kiwi("onCreate", listOf(ACTIVITY, "Z"), "V"),
            kiwi("onCreate", listOf(SERVICE, "Z"), "V"),
            kiwi("onStart", listOf(ACTIVITY), "V"),
            kiwi("onResume", listOf(ACTIVITY), "V"),
            kiwi("onPause", listOf(ACTIVITY), "V"),
            kiwi("onStop", listOf(ACTIVITY), "V"),
            kiwi("onDestroy", listOf(ACTIVITY), "V"),
            kiwi("onDestroy", listOf(SERVICE), "V"),
            kiwi("onWindowFocusChanged", listOf(ACTIVITY, "Z"), "V"),
        ).forEach { it.method.addInstructions(0, "return-void") }

        kiwi("onActivityResult", listOf(ACTIVITY, "I", "I", "Landroid/content/Intent;"), "Z")
            .method.addInstructions(
                0,
                """
                    const/4 v0, 0x0
                    return v0
                """,
            )

        kiwi("onCreateDialog", listOf(ACTIVITY, "I"), "Landroid/app/Dialog;")
            .method.addInstructions(
                0,
                """
                    const/4 v0, 0x0
                    return-object v0
                """,
            )
    }
}
