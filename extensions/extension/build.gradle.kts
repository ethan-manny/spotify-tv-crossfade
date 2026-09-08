extension {
    name = "extensions/extension.mpe"
}

android {
    namespace = "dev.spotifytv.crossfade.extension"
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")   // real org.json for JVM tests (android.jar's is a stub)
}
