package com.crossscale.nativelib

/**
 * Coordinator-side JNI facade (connection handshake + identity load).
 *
 * The externals must stay on this class: the C++ bridge exports them as
 * Java_com_crossscale_nativelib_NativeWorker_* symbols.
 */
object NativeWorker {
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
    external fun nativeRequestConnection(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        wantsEncryptedStream: Boolean
    ): String?

    /**
     * 拉取 PC 模型库列表。每次调用走一次独立连接（握手+配对+列表帧）。
     * 成功返回逐行记录 "name\tscale\tsize"；失败返回 null（错误见 NCNNEngine.nativeGetLastError）。
     */
    external fun nativeListPCModels(
        host: String,
        port: Int,
        peerPubKeyB64: String
    ): String?

    /** 上传手机本地模型（param+bin）到 PC 模型库。 */
    external fun nativeUploadModelToPC(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        modelName: String,
        scaleFactor: Int,
        paramPath: String,
        binPath: String
    ): Boolean

    /** 从 PC 模型库下载模型到指定落盘路径。 */
    external fun nativeDownloadModelFromPC(
        host: String,
        port: Int,
        peerPubKeyB64: String,
        modelName: String,
        destParamPath: String,
        destBinPath: String
    ): Boolean

    external fun nativeLoadIdentity(identityPath: String): Boolean
}
