// Minimal Android app proving the game-code pipeline on-device: pick the game's exefs (SAF),
// generate C with libnativegen (JNI), compile it with the bundled zig, dlopen the result.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "PowerMoonRecomp"
include(":app")
