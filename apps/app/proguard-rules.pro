# CrossScale ProGuard Rules
# These rules prevent obfuscation of classes and methods that are accessed
# via JNI or reflection, ensuring native code and serialization work correctly
# in release builds.

# Keep all JNI native methods - required for native C++ bridge
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep NativeWorker and NCNNEngine classes that are called from JNI
-keep class com.crossscale.nativelib.NativeWorker { *; }
-keep class com.crossscale.nativelib.NCNNEngine { *; }

# Keep data model classes used in serialization
-keep class com.crossscale.data.model.** { *; }

# Keep kotlinx.serialization classes
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.AnnotationsKt

-keepclassmembers class kotlinx.serialization.json.** {
    *** Companion;
}
-keepclasseswithmembers class kotlinx.serialization.json.** {
    kotlinx.serialization.KSerializer serializer(...);
}

-keep,includedescriptorclasses class com.crossscale.**$$serializer { *; }
-keepclassmembers class com.crossscale.** {
    *** Companion;
}
-keepclasseswithmembers class com.crossscale.** {
    kotlinx.serialization.KSerializer serializer(...);
}

# Keep EncryptedSharedPreferences and security-crypto internals
-keep class androidx.security.crypto.** { *; }

# Tink (via security-crypto) references these annotations; they are compile-only
# and used to arrive transitively via ML Kit. Compile-only: ignore at R8 time.
-dontwarn com.google.errorprone.annotations.CanIgnoreReturnValue
-dontwarn com.google.errorprone.annotations.CheckReturnValue
-dontwarn com.google.errorprone.annotations.Immutable
-dontwarn com.google.errorprone.annotations.RestrictedApi
-dontwarn javax.annotation.Nullable
-dontwarn javax.annotation.concurrent.GuardedBy
