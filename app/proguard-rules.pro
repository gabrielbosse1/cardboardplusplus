# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.

# Keep Cardboard SDK classes
-keep class com.google.vr.** { *; }
-keep class com.google.android.** { *; }

# Keep MediaPipe classes
-keep class com.google.mediapipe.** { *; }
-keepclassmembers class com.google.mediapipe.** { *; }

# Keep protobuf classes
-keep class * extends com.google.protobuf.GeneratedMessageLite { *; }

# Keep native methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Uncomment this to preserve the line number information for
# debugging stack traces.
-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
-renamesourcefileattribute SourceFile
