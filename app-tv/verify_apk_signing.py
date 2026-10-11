#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""读出 APK 的签名证书；是 debug 密钥就判失败。

为什么非要有这一步：签名配置一旦落空（路径写错、Secrets 没递进可复用工作流、
口令不对），gradle **不会报错**，它静默退回 debug 签名。而 debug 密钥库是每个
runner 构建时现生成的，于是每个版本签名都不同 —— 用户装新版必须先卸载，卸载
又会清掉配置与配对密钥。这种包平时"能用"，只在升级那一刻暴露，所以当面把证书
拆出来看一眼，比等用户反馈便宜得多。

只解 v2 签名块：minSdk 26，AGP 不会再生成 v1（META-INF/*.RSA）签名。

用法：
    python3 verify_apk_signing.py adisplay-android-tv.apk
"""
import os
import struct
import subprocess
import sys

# 签名块的魔数，紧贴在 ZIP 中央目录之前。
SIGNING_BLOCK_MAGIC = b"APK Sig Block 42"
# v2 签名方案的块 ID；v3 是 0xf05368c0，不看。
V2_BLOCK_ID = 0x7109871A
# AGP 自动生成的 debug 密钥库主题，见 ~/.android/debug.keystore。
DEBUG_MARK = "Android Debug"


def read_length_prefixed(buf, offset):
    """v2 块里所有列表都是 uint32 长度前缀 + 内容。"""
    length = struct.unpack_from("<I", buf, offset)[0]
    offset += 4
    return buf[offset:offset + length], offset + length


def certificates_of(v2_block):
    """v2 块 -> 签名者 -> 被签名数据 -> 证书列表。"""
    signers, _ = read_length_prefixed(v2_block, 0)
    certs = []
    offset = 0
    while offset < len(signers):
        signer, offset = read_length_prefixed(signers, offset)
        signed_data, _ = read_length_prefixed(signer, 0)
        # 被签名数据内部依次是：摘要列表、证书列表、附加属性。
        _, inner = read_length_prefixed(signed_data, 0)
        cert_seq, inner = read_length_prefixed(signed_data, inner)
        cert_offset = 0
        while cert_offset < len(cert_seq):
            cert, cert_offset = read_length_prefixed(cert_seq, cert_offset)
            certs.append(cert)
    return certs


def extract_v2_block(path):
    with open(path, "rb") as handle:
        data = handle.read()

    eocd = data.rfind(b"PK\x05\x06")
    if eocd < 0:
        raise ValueError("不是有效的 ZIP/APK：找不到中央目录结束记录")
    # EOCD 偏移 16 处是中央目录的起始偏移。
    central_dir = struct.unpack_from("<I", data, eocd + 16)[0]

    if data[central_dir - 16:central_dir] != SIGNING_BLOCK_MAGIC:
        raise ValueError("APK 里没有签名块 —— 这个包没签名？")
    # 块尾还有一个 8 字节长度，与块首那个相同，用它反推块起点。
    size = struct.unpack_from("<Q", data, central_dir - 24)[0]
    start = central_dir - size - 8
    if struct.unpack_from("<Q", data, start)[0] != size:
        raise ValueError("签名块首尾长度不一致，文件可能被改过")

    pairs = data[start + 8:central_dir - 24]
    offset = 0
    while offset < len(pairs):
        length = struct.unpack_from("<Q", pairs, offset)[0]
        offset += 8
        block_id = struct.unpack_from("<I", pairs, offset)[0]
        value = pairs[offset + 4:offset + length]
        offset += length
        if block_id == V2_BLOCK_ID:
            return value
    raise ValueError("签名块里没有 v2 方案")


def describe(cert_der, index):
    """证书主题与指纹交给 openssl 印，比在 python 里解 ASN.1 省事。"""
    path = "apk-cert-%d.der" % index
    with open(path, "wb") as handle:
        handle.write(cert_der)
    try:
        out = subprocess.check_output([
            "openssl", "x509", "-inform", "DER", "-in", path,
            "-noout", "-subject", "-fingerprint", "-sha256",
        ], stderr=subprocess.STDOUT)
    finally:
        os.remove(path)
    return out.decode("utf-8", "replace").strip()


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("用法：verify_apk_signing.py <apk>\n")
        return 2

    try:
        v2_block = extract_v2_block(argv[1])
        descriptions = [describe(cert, i) for i, cert in enumerate(certificates_of(v2_block))]
    except (ValueError, OSError, struct.error) as error:
        sys.stderr.write("读签名失败：%s\n" % error)
        return 1

    for text in descriptions:
        sys.stdout.write(text + "\n")

    for text in descriptions:
        if DEBUG_MARK in text:
            sys.stderr.write(
                "签名用的是 debug 密钥库 —— 它由每个 runner 构建时现生成，"
                "版本之间不固定，用户覆盖安装会失败。\n"
                "查 app-tv/app/build.gradle.kts 的签名段：keyAlias、口令、"
                "storeFile 指的密钥库是不是 app-tv/fallback-signing.p12。\n"
            )
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
