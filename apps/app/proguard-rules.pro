# Upix ProGuard Rules
# These rules prevent obfuscation of classes and methods that are accessed
# via JNI or reflection, ensuring native code and serialization work correctly
# in release builds.

# Keep the names of every class containing JNI native methods. This covers
# NativeWorker and NCNNEngine: the Java_com_upix_nativelib_* exports in
# jni_bridge.cpp resolve classes and methods by name. Their non-native
# members are only referenced from Kotlin, which R8 traces statically
# (jni_bridge.cpp reflects into nothing but java/lang/String).
-keepclasseswithmembernames class * {
    native <methods>;
}

# kotlinx.serialization (official template, com.upix substituted). The
# @Serializable DTOs live in com.upix.data.repository (queue/checkpoint);
# kotlinx-serialization also ships these as R8 consumer rules — kept here
# as a belt-and-suspenders safety net.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.AnnotationsKt

-keepclassmembers class kotlinx.serialization.json.** {
    *** Companion;
}
-keepclasseswithmembers class kotlinx.serialization.json.** {
    kotlinx.serialization.KSerializer serializer(...);
}

-keep,includedescriptorclasses class com.upix.**$$serializer { *; }
-keepclassmembers class com.upix.** {
    *** Companion;
}
-keepclasseswithmembers class com.upix.** {
    kotlinx.serialization.KSerializer serializer(...);
}

# Tink (via androidx.security:security-crypto -> tink-android) references
# these compile-only annotations, which are absent from the runtime classpath;
# Tink bundles no -dontwarn rules of its own (only protobuf.pro), so silence
# them here.
-dontwarn com.google.errorprone.annotations.CanIgnoreReturnValue
-dontwarn com.google.errorprone.annotations.CheckReturnValue
-dontwarn com.google.errorprone.annotations.Immutable
-dontwarn com.google.errorprone.annotations.RestrictedApi
-dontwarn javax.annotation.Nullable
-dontwarn javax.annotation.concurrent.GuardedBy
