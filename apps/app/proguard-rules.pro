# CrossScale ProGuard Rules
# These rules prevent obfuscation of classes and methods that are accessed
# via JNI or reflection, ensuring native code and serialization work correctly
# in release builds.

# Keep all JNI native methods - required for native C++ bridge
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep NativeWorker and NCNNEngine classes that are called from JNI
-keep class com.crossscale.native.NativeWorker { *; }
-keep class com.crossscale.native.NCNNEngine { *; }

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

# Keep Ktor client classes
-keep class io.ktor.** { *; }
-dontwarn io.ktor.**
