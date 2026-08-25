package com.upix.data.repository

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class InputSourceSpecTest {
    @Test
    fun encodeReturnsNullWhenListIsEmpty() {
        assertNull(InputSourceSpec.encode(emptyList()))
    }

    @Test
    fun encodeReturnsRawValueWhenOnlyOneSource() {
        val source = "content://media/images/1"
        assertEquals(source, InputSourceSpec.encode(listOf(source)))
    }

    @Test
    fun encodeAndDecodeMultiSourceRoundTrip() {
        val encoded = InputSourceSpec.encode(
            listOf(
                " content://images/1 ",
                "content://images/2",
                "content://images/1"
            )
        )

        checkNotNull(encoded)
        val decoded = InputSourceSpec.decode(encoded)

        assertEquals(
            listOf("content://images/1", "content://images/2"),
            decoded
        )
    }

    @Test
    fun decodeReturnsNullForSingleSourceSpec() {
        assertNull(InputSourceSpec.decode("/tmp/image.png"))
    }

    @Test
    fun decodeInvalidPayloadReturnsEmptyList() {
        val decoded = InputSourceSpec.decode("upix://multi:not-json")
        assertTrue(decoded?.isEmpty() == true)
    }
}
