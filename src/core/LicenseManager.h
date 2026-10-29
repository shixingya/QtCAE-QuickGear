#pragma once

#include <QByteArray>
#include <QDate>
#include <QString>

/** 到期前多少天开始提醒（签发侧 audit 与客户端徽标共用同一个阈值） */
constexpr int kExpiryWarnDays = 30;

/**
 * 授权文件的判定结论。
 *
 * 为什么要区分到这么细：`valid` 只有是/否两态，而“签名有效但已过期”与
 * “签名对不上”是两件完全不同的事——前者是续费对象，后者要查是谁改过文件。
 * 签发侧 audit 需要按这个区分做续期清单，客户端也要据此给出不同措辞。
 */
enum class LicenseState
{
    Missing,        // 没有授权文件
    Unreadable,     // 打不开 / JSON 结构错误
    BadSignature,   // 签名对不上：被改过，或不是本工具签发的
    ForeignMachine, // 签名有效，但绑的不是这台机器
    Expired,        // 签名有效，但已过到期日
    Valid           // 全部通过
};

/**
 * 授权信息（由本地授权文件解析得到）。
 */
struct LicenseInfo
{
    bool         valid            = false;  // 结构、机器码、有效期都通过
    bool         signatureChecked = false;  // 是否完成密码学验签
    LicenseState state            = LicenseState::Missing;
    QString      machineCode;               // 本机机器码
    QString      boundMachine;              // 授权文件里绑定的机器码（审计他机授权时看这个）
    QString      licensee;                  // 授权对象
    QString      edition;                   // pro / trial
    QString      alg;                       // 签名算法：ed25519 / hmac（旧文件无此字段）
    QDate        issued;
    QDate        expire;
    /**
     * 距到期天数：expire 有效时 = 今天到 expire 的天数，负数表示已过期 N 天；
     * expire 无效（永久授权）时固定为 -1，需配合 expire.isValid() 判断。
     */
    int          daysLeft         = -1;
    QString      message;                   // 给用户看的一句话状态

    /** 有到期日、且剩余天数落在提醒窗口内 */
    bool expiringSoon() const
    {
        return valid && expire.isValid() && daysLeft >= 0 && daysLeft <= kExpiryWarnDays;
    }
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
 *   4. 签名支持两种算法：
 *      - ed25519（推荐）：非对称。客户端只内置 32 字节公钥，拆开 exe 也签不出授权；
 *      - hmac-sha256（兼容）：v0.3 及以前的文件仍能用，但密钥编在客户端里，只挡随手改。
 *      规范化串 `machine|licensee|edition|issued|expire` 两种算法共用，
 *      避免 JSON 序列化差异导致验签不稳定。机器码在串内统一归一化。
 *   5. 新签发的授权应一律用 ed25519；hmac 路径只为“旧的还在用”保留，
 *      audit 会把它们标出来（ALG=hmac），方便逐张换新。
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

    /** 解析指定授权文件（含机器码绑定校验，即“这张授权能不能在这台机器上用”） */
    LicenseInfo loadFromFile(const QString &path) const;

    /**
     * 解析授权文件，可选择是否校验机器码绑定。
     *
     * checkMachine = false 供签发侧审计使用：客户机器码必然与自己的不同，
     * 但仍要验签、仍要算到期，才能分出“该续费”与“文件被改过”。
     */
    static LicenseInfo parseFile(const QString &path, bool checkMachine);

    /**
     * 把外部授权文件安装到默认路径，成功返回 true。
     *
     * 先解析校验再落盘：签名不对 / 绑定他机 / 文件损坏一律拒绝，原授权保持不动。
     * 替换时先写 .incoming 再改名，在新文件就位前不删旧文件。
     *
     * 返回 true 时 error 仍可能带提示（例如“已导入但已到期”），调用方应当显示。
     *
     * targetPath 留空则装到默认位置；自检传入临时路径，保证不碰用户真授权。
     */
    bool installFrom(const QString &srcPath, QString *error = nullptr,
                     const QString &targetPath = QString()) const;

    /** 状态名（ASCII，给脚本 grep 用） */
    static QString stateName(LicenseState state);

    /** 剩余天数的人话表述：剩 12 天 / 今日到期 / 已过期 3 天 / 永久有效 */
    static QString expiryText(const LicenseInfo &info);

    /**
     * 机器码归一化：去空白、转大写、缺连字符的 32 位十六进制补齐分组。
     * 签发与验签都经过它，客户把机器码抄成一行或漏掉横杠也不会白跑一趟。
     */
    static QString normalizeMachineCode(const QString &raw);

    /**
     * 机器码是否长得像我们自己生成的码：32 位十六进制、按 4 位分组（共 39 字符）。
     * 签发前拦一道——机器码抄错一个字符，客户装上去只会看到「与本机不匹配」，
     * 而双方都会以为是对方的问题。
     */
    static bool machineCodeLooksValid(const QString &raw);

    /**
     * 写出一份授权文件（签名、JSON 结构都在这里，签发工具不再自己拼）。
     *
     * signingSeed 为 32 字节 Ed25519 种子时用公钥签名（推荐）；
     * 留空则退回 HMAC（仅为了自检与旧流程能跑，新授权不要用）。
     * signatureBase64 非空时回传签名，供批量交付单记录。
     */
    static bool writeLicenseFile(const QString &path, const QString &machine,
                                 const QString &licensee, const QString &edition,
                                 const QDate &issued, const QDate &expire,
                                 QString *signatureBase64 = nullptr,
                                 QString *error = nullptr,
                                 const QByteArray &signingSeed = QByteArray());

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

    /**
     * 编译期注入的 Ed25519 公钥（CMake: QTG_LICENSE_PUBKEY，base64）。
     * 为空表示这个构建不支持公钥验签，遇到 ed25519 授权会明确报错而不是默默放行。
     */
    static QByteArray signingPublicKey();

    /** 用内置公钥验 64 字节 Ed25519 签名 */
    static bool verifyEd25519(const QString &canonical, const QByteArray &signature64);

    /**
     * 用指定公钥验签（32 字节）。与内置公钥无关，给自检与“换公钥后旧授权失效”
     * 这类验证用；客户端运行时应当用 verifyEd25519。
     */
    static bool verifyEd25519With(const QByteArray &publicKey, const QString &canonical,
                                  const QByteArray &signature64);

    /** 签名算法的可读名称，给界面、报告与命令行共用一份措辞 */
    static QString algName(const QString &alg);

private:
    static QByteArray rawMachineId();
    static QString formatCode(const QString &hex);
};
