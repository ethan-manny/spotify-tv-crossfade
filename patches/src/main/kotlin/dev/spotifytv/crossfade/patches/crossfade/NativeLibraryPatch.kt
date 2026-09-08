package dev.spotifytv.crossfade.patches.crossfade

import app.morphe.patcher.patch.PatchException
import app.morphe.patcher.patch.ResourcePatch
import app.morphe.patcher.patch.resourcePatch
import dev.spotifytv.crossfade.patches.shared.Constants.COMPATIBILITY_SPOTIFY_TV

private const val ESDK_LIB = "lib/armeabi-v7a/libspotify_tv_jni.so"
private const val XFADE_LIB = "lib/armeabi-v7a/libxfade.so"
private const val XFADE_RESOURCE = "/xfade/armeabi-v7a/libxfade.so"

/** libOpenSLES.so plus its terminator: 15 bytes. The replacement is padded to the same length. */
private val OLD_DEPENDENCY = "libOpenSLES.so\u0000".toByteArray(Charsets.US_ASCII)
private val NEW_DEPENDENCY = "libxfade.so\u0000\u0000\u0000\u0000".toByteArray(Charsets.US_ASCII)

/**
 * Internal: adds libxfade.so to the APK and points the eSDK's OpenSL ES dependency at it.
 */
val nativeLibraryPatch: ResourcePatch = resourcePatch {
    compatibleWith(COMPATIBILITY_SPOTIFY_TV)
    dependsOn(manifestPatch)

    execute {
        // 1. Rename the dependency string in place.
        val esdkFile = get(ESDK_LIB)
        if (!esdkFile.exists()) throw PatchException("$ESDK_LIB not found in the APK")
        val bytes = esdkFile.readBytes()
        val hits = occurrences(bytes, OLD_DEPENDENCY)
        if (hits.size != 1) {
            throw PatchException("Expected exactly one 'libOpenSLES.so' dependency string in $ESDK_LIB, found ${hits.size}")
        }
        NEW_DEPENDENCY.copyInto(bytes, hits[0])
        esdkFile.writeBytes(bytes)

        // 2. Add our library from the bundled resources.
        // Note: nativeLibraryPatch.javaClass would resolve to app.morphe.patcher.patch.ResourcePatch,
        // which is loaded by a different classloader than this patch bundle's own classes and cannot
        // see resources packaged inside our .mpp. Anchor the lookup on a class from this compilation
        // unit instead so the resource resolves against the bundle's own classloader.
        val stream = object {}.javaClass.getResourceAsStream(XFADE_RESOURCE)
            ?: throw PatchException("Bundled $XFADE_RESOURCE missing; run scripts/build-native.sh before building the bundle")
        val target = get(XFADE_LIB)
        target.parentFile?.mkdirs()
        stream.use { input -> target.outputStream().use { output -> input.copyTo(output) } }
    }
}

private fun occurrences(haystack: ByteArray, needle: ByteArray): List<Int> {
    val result = mutableListOf<Int>()
    var i = 0
    outer@ while (i <= haystack.size - needle.size) {
        for (j in needle.indices) if (haystack[i + j] != needle[j]) { i++; continue@outer }
        result += i
        i += needle.size
    }
    return result
}
