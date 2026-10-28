#pragma once

#include <QByteArray>
#include <QDate>
#include <QString>

/**
 * 授权信息（由本地授权文件解析得到）。
 */
struct LicenseInfo
{
    bool    valid            = false;   // 结构、机器码、有效期都通过
    bool    signatureChecked = false;   // 是否完成密码学验签
    QString machineCode;                // 授权绑定的机器码
    QString licensee;                   // 授权对象
    QString edition;                    // pro / trial
    QDate   issued;
    QDate   expire;
    QString message;                    // 给用户看的一句话状态
};

/**
 * 本机机器码与离线授权管理。
 *
 * 设计要点：
 *   1. 机器码 = SHA-256(机器唯一标识 + 系统信息 + Windows MachineGuid) 截取 32 位十六进制，
 *      按 4 位分组显示（XXXX-XXXX-...），与网页端设备码格式保持一致，便于人工核对与沟通。
 *   2. 机器码在客户端本地计算，不上传；网页端无法得知客户端机器码，
 *      因此**网页设备码只用于沟通定位，激活以客户端机器码为准**。
 *   3. 授权文件为 JSON，放在用户配置目录（不写入安装目录，避免 UAC 问题）。
 *   4. 签名采用 HMAC-SHA256（编译期注入的共享密钥），签名覆盖的规范化串为
 *      `machine|licensee|edition|issued|expire`，避免 JSON 序列化差异导致验签不稳定。
 *   5. ⚠️ 共享密钥方案的局限：密钥编进客户端后理论上可被逆向获取（开源版默认密钥
 *      更是公开的）。它能挡住“拿到文件就能用”的随手修改，挡不住专门的破解。
 *      正式版应将 verifySignature() 换成闭源模块里的 Ed25519 公钥验签（私钥只在签发侧），
 *      接口保持不变，其余流程不用改。
 */
class LicenseManager
{
public:
    LicenseManager() = default;

    /** 本机机器码（带缓存），格式 XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX */
    static QString machineCode();

    /** 默认授权文件路径：<AppData>/QtCAE-QuickGear/license.qglic */
    static QString defaultLicensePath();

    /** 读取默认路径下的授权；文件不存在时返回试用状态 */
    LicenseInfo current() const;

    /** 解析指定授权文件 */
    LicenseInfo loadFromFile(const QString &path) const;

    /** 把外部授权文件安装到默认路径，成功返回 true */
    bool installFrom(const QString &srcPath, QString *error = nullptr) const;

    /**
     * 规范化签名串：machine|licensee|edition|issued|expire
     * 签发与验签必须使用同一函数，避免两边拼串规则不一致。
     */
    static QString canonicalString(const QString &machine, const QString &licensee,
                                   const QString &edition, const QDate &issued,
                                   const QDate &expire);

    /** HMAC-SHA256 签名（原始 32 字节）；写入授权文件时用 base64 */
    static QByteArray signPayload(const QString &canonical);

    /** 校验 base64 签名，内部做常时间比较 */
    static bool verifySignature(const QString &canonical, const QByteArray &signatureBase64);

private:
    static QByteArray rawMachineId();
    static QString formatCode(const QString &hex);
};
