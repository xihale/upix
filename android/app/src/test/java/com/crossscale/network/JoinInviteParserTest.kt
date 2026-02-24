package com.crossscale.network

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class JoinInviteParserTest {

    @Test
    fun parse_cli_qr_json_payload() {
        val payload = """{"v":1,"ip":"192.168.1.100","port":9527,"key":"abc123"}"""

        val invite = JoinInviteParser.parse(payload)

        assertNotNull(invite)
        assertEquals("192.168.1.100", invite?.host)
        assertEquals(9527, invite?.port)
        assertEquals("abc123", invite?.authKey)
    }

    @Test
    fun parse_short_string_payload() {
        val invite = JoinInviteParser.parse("192.168.1.8:9527#key-value")

        assertNotNull(invite)
        assertEquals("192.168.1.8", invite?.host)
        assertEquals(9527, invite?.port)
        assertEquals("key-value", invite?.authKey)
    }

    @Test
    fun parse_uri_payload() {
        val invite = JoinInviteParser.parse("crossscale://join?host=10.0.0.11&port=12000&key=token")

        assertNotNull(invite)
        assertEquals("10.0.0.11", invite?.host)
        assertEquals(12000, invite?.port)
        assertEquals("token", invite?.authKey)
    }

    @Test
    fun parse_invalid_payload_returns_null() {
        assertNull(JoinInviteParser.parse("not-a-valid-invite"))
        assertNull(JoinInviteParser.parse("""{"port":9527}"""))
        assertNull(JoinInviteParser.parse("192.168.1.9:70000#x"))
    }
}
