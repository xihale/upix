package io.upix.data.repository

import org.junit.Assert.assertEquals
import org.junit.Test

class OutputFormatTest {
    @Test
    fun outputFormatForExtensionRecognizesAvif() {
        assertEquals("avif", outputFormatForExtension("avif"))
        assertEquals("avif", outputFormatForExtension(".AVIF"))
        assertEquals("png", outputFormatForExtension("gif"))
    }

    @Test
    fun followAvifFallsBackToPngWhenLocal() {
        assertEquals(
            "png",
            resolveOutputExtension(
                displayName = "photo.avif",
                requested = OutputImageFormat.FOLLOW,
                overwriteOriginal = false,
                writeAvif = false
            )
        )
    }

    @Test
    fun followAvifKeepsAvifWhenRemote() {
        assertEquals(
            "avif",
            resolveOutputExtension(
                displayName = "photo.avif",
                requested = OutputImageFormat.FOLLOW,
                overwriteOriginal = false,
                writeAvif = true
            )
        )
    }

    @Test
    fun forcedAvifKeepsExtensionEvenLocally() {
        assertEquals(
            "avif",
            resolveOutputExtension(
                displayName = "photo.png",
                requested = OutputImageFormat.AVIF,
                overwriteOriginal = false,
                writeAvif = false
            )
        )
    }

    @Test
    fun overwriteKeepsOriginalAvifExtension() {
        assertEquals(
            "avif",
            resolveOutputExtension(
                displayName = "photo.avif",
                requested = OutputImageFormat.PNG,
                overwriteOriginal = true,
                writeAvif = false
            )
        )
    }
}
