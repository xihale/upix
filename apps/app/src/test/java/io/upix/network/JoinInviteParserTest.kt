package io.upix.network

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class JoinInviteParserTest {

    @Test
    fun parse_cli_qr_json_payload() {
        val payload = """{"v":2,"ip":"192.168.1.100","port":9527,"pk":"base64key","fp":"AB12CD34"}"""

        val invite = JoinInviteParser.parse(payload)

        assertNotNull(invite)
        assertEquals("192.168.1.100", invite?.host)
        assertEquals(9527, invite?.port)
        assertEquals("base64key", invite?.identityPubKey)
        assertEquals("AB12CD34", invite?.fingerprint)
    }

    @Test
    fun parse_short_string_payload() {
        val invite = JoinInviteParser.parse("192.168.1.8:9527")

        assertNotNull(invite)
        assertEquals("192.168.1.8", invite?.host)
        assertEquals(9527, invite?.port)
        assertEquals("", invite?.identityPubKey)
        assertNull(invite?.fingerprint)
    }

    @Test
    fun parse_uri_payload() {
        val invite = JoinInviteParser.parse("upix://join?host=10.0.0.11&port=12000&pk=token")

        assertNotNull(invite)
        assertEquals("10.0.0.11", invite?.host)
        assertEquals(12000, invite?.port)
        assertEquals("token", invite?.identityPubKey)
    }

    @Test
    fun parse_invalid_payload_returns_null() {
        assertNull(JoinInviteParser.parse("not-a-valid-invite"))
        assertNull(JoinInviteParser.parse("""{"port":9527}"""))
        assertNull(JoinInviteParser.parse("192.168.1.9:70000#x"))
    }

    @Test
    fun parse_malformed_percent_encoding_returns_null() {
        // 非法百分号序列不得抛异常（曾评估为恶意 QR 闪退路径）：
        // java.net.URI 构造本身严格校验 query 转义，坏序列在 URI() 即被
        // runCatching 拦下返回 null；parseQueryParams 的逐对 runCatching
        // 是纵深防御（防未来换成宽松解析如 android.net.Uri 时回归）。
        assertNull(JoinInviteParser.parse("upix://join?pk=%zz&port=12000"))
        assertNull(JoinInviteParser.parse("upix://join?pk=%&host=10.0.0.11&port=12000"))
        assertNull(JoinInviteParser.parse("upix://join?host=10.0.0.11&port=12000&pk=ok&junk=%zz"))
        // 合法转义序列正常解码。
        val invite = JoinInviteParser.parse("upix://join?host=10.0.0.11&port=12000&pk=a%20b")
        assertNotNull(invite)
        assertEquals("a b", invite?.identityPubKey)
    }
}
