package dev.spotifytv.crossfade.patches.crossfade

import app.morphe.patcher.extensions.InstructionExtensions.addInstructions
import app.morphe.patcher.extensions.InstructionExtensions.addInstructionsWithLabels
import app.morphe.patcher.patch.PatchException
import app.morphe.patcher.patch.bytecodePatch
import dev.spotifytv.crossfade.patches.shared.Constants.COMPATIBILITY_SPOTIFY_TV
import dev.spotifytv.crossfade.patches.shared.Constants.HOOKS_CLASS

@Suppress("unused")
val crossfadePatch = bytecodePatch(
    name = "Crossfade",
    description = "Crossfade between songs (0-12 s). Long-press Menu in the app to set the length.",
) {
    compatibleWith(COMPATIBILITY_SPOTIFY_TV)
    dependsOn(nativeLibraryPatch)
    extendWith("extensions/extension.mpe")

    execute {
        // 1. Load the library and push the saved length before the eSDK loads. p0 is the Application (a Context).
        ApplicationOnCreateFingerprint.method.addInstructions(
            0,
            "invoke-static { p0 }, $HOOKS_CLASS->init(Landroid/content/Context;)V",
        )

        // 2. Hand every metadata JSON to the boundary model. The lambda is static, so parameter i is register p<i>.
        metadataLambdaFingerprint.method.apply {
            val json = parameters.indexOfFirst { it.type == "Ljava/lang/String;" }
            if (json < 0) throw PatchException("metadata lambda has no String parameter")
            addInstructions(0, "invoke-static { p$json }, $HOOKS_CLASS->onMetadataJson(Ljava/lang/String;)V")
        }

        // 3. Let the long-press detector see every key first; return true when it consumes the event.
        dispatchKeyEventFingerprint.method.apply {
            val impl = implementation ?: throw PatchException("dispatchKeyEvent has no implementation")
            val ins = parameterTypes.size + 1   // instance method: 'this' plus the KeyEvent
            // We need one local (v0). dexlib2's MutableMethodImplementation exposes no register-count
            // setter, so assert the method already has one instead of growing the frame.
            if (impl.registerCount - ins < 1) {
                throw PatchException("dispatchKeyEvent has no free local register (registers=${impl.registerCount}, ins=$ins)")
            }
            addInstructionsWithLabels(
                0,
                """
                    invoke-static { p0, p1 }, $HOOKS_CLASS->onKeyEvent(Landroid/app/Activity;Landroid/view/KeyEvent;)Z
                    move-result v0
                    if-eqz v0, :xfade_pass
                    return v0
                    :xfade_pass
                    nop
                """,
            )
        }
    }
}
