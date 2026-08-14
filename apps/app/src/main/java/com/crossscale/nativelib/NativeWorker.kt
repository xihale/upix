package com.crossscale.nativelib

class NativeWorker {
    init {
        System.loadLibrary("crossscale-native")
    }

    /**
     * 新连接流程: ECDH 握手 + CONNECT_REQUEST/RESPONSE 配对(不传输图像)。
     * PC 侧对可信 peer 自动批准, 首次 peer 弹 [y/n] 提示。
     *
     * peerPubKeyB64 为空时进入 TOFU 模式: 握手期间学到 PC 身份公钥, 返回其 base64。
     * 非空时严格 MITM 校验, 成功返回传入公钥。失败返回 null。
     * 调用方应将返回的公钥持久化, 以便后续连接/图像处理恢复严格校验。
     */
    fun requestConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean
    ): String? {
        return nativeRequestConnection(host, port, peerPubKeyB64, wantsEncryptedStream)
    }

    fun loadIdentity(identityPath: String): Boolean = nativeLoadIdentity(identityPath)

    private external fun nativeRequestConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean
    ): String?

    private external fun nativeLoadIdentity(identityPath: String): Boolean
}
