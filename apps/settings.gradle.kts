pluginManagement {
    // Aliyun first locally (and under nektos/act). GitHub-hosted runners are
    // outside CN and stall or fail on maven.aliyun.com — skip the mirror there.
    // pluginManagement is an isolated scope: cannot see settings-script vals.
    val onGitHubHosted =
        System.getenv("GITHUB_ACTIONS") == "true" && System.getenv("ACT") != "true"
    repositories {
        if (!onGitHubHosted) {
            maven("https://maven.aliyun.com/repository/gradle-plugin/")
            maven("https://maven.aliyun.com/repository/google/")
            maven("https://maven.aliyun.com/repository/public/")
        }
        gradlePluginPortal()
        google()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    val onGitHubHosted =
        System.getenv("GITHUB_ACTIONS") == "true" && System.getenv("ACT") != "true"
    repositories {
        if (!onGitHubHosted) {
            maven("https://maven.aliyun.com/repository/google/")
            maven("https://maven.aliyun.com/repository/public/")
            maven("https://maven.aliyun.com/repository/gradle-plugin/")
        }
        google()
        mavenCentral()
    }
}

rootProject.name = "Upix"
include(":app")
