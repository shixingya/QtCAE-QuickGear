#pragma once

#include <QByteArray>
#include <QString>

/**
 * Ed25519：授权文件的非对称签名。
 *
 * 为什么值得多这一层：原来的 HMAC 用的是编进客户端的共享密钥，
 * 谁把 exe 拆开谁就能自己签授权。换成公钥验签以后，客户端里只有公钥，
 * 拿它签不出任何东西——授权文件发出去才真正“不怕被逆向”。
 *
 * 实现说明：
 *  - 曲线运算用 5×51 位域元素（unsigned __int128 做累加），不依赖第三方库；
 *  - 哈希直接复用 Qt 的 SHA-512，不自己实现；
 *  - 这是教学级/产品级的自洽实现，已用 RFC 8032 §7.1 的官方向量逐字节验证。
 *    与 ref10 相比不做常数时间保证：私钥只在我方签发机上使用，不对外暴露侧信道。
 *
 * 密钥格式：seed = 32 字节随机数；公钥 = 32 字节；签名 = 64 字节（R || S）。
 */
namespace Ed25519
{
/** 由 32 字节种子推导公钥（32 字节） */
bool derivePublicKey(const QByteArray &seed, QByteArray *publicKey, QString *error = nullptr);

/** 用 32 字节种子对任意消息签名，输出 64 字节签名 */
bool sign(const QByteArray &message, const QByteArray &seed, QByteArray *signature,
          QString *error = nullptr);

/** 验签；公钥或签名长度不对、S 非规范、R/A 不在曲线上，一律返回 false */
bool verify(const QByteArray &message, const QByteArray &signature, const QByteArray &publicKey);

/** 把 32 字节密钥编成 base64（便于放进配置文件/命令行） */
QString encodeKey(const QByteArray &key);

/** 解析 base64 密钥；长度不是 32 字节时返回空并写明原因 */
QByteArray decodeKey(const QString &text, QString *error = nullptr);

/** 生成 32 字节种子（QRandomGenerator::system()） */
QByteArray randomSeed();
} // namespace Ed25519
