group = "dev.spotifytv.crossfade"

patches {
    about {
        name = "Spotify TV Crossfade"
        description = "Adds crossfade between songs to the Spotify TV app on Fire TV / Android TV."
        source = "https://github.com/ethan-manny/spotify-tv-crossfade"
        author = "Ethan Mansfield"
        contact = "ethanmansfield2001@gmail.com"
        website = "https://github.com/ethan-manny/spotify-tv-crossfade"
        license = "GPLv3"
    }
}

// Separate configuration so gson is available at runtime for the
// generatePatchesList task but never bundled into the APK.
val patchListGeneratorClasspath = configurations.create("patchListGeneratorClasspath")

dependencies {
    compileOnly(libs.gson)
    patchListGeneratorClasspath(libs.gson)
}

tasks {
    register<JavaExec>("generatePatchesList") {
        description = "Build patch with patch list"

        dependsOn(build)

        classpath = sourceSets["main"].runtimeClasspath + patchListGeneratorClasspath
        mainClass.set("util.PatchListGeneratorKt")
    }

    // Used by gradle-semantic-release-plugin.
    publish {
        dependsOn("generatePatchesList")
    }
}
