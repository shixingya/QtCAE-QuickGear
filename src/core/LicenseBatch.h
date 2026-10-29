#pragma once

#include <QDate>
#include <QString>
#include <QStringList>
#include <QVector>

/**
 * 一条签发订单。字段与台账 CSV 的列一一对应。
 */
struct BatchOrder
{
    QString machine;                // 客户机器码（连字符可省，签发时统一归一化）
    QString licensee;               // 授权对象
    QString edition = QStringLiteral("pro");
    QDate   issued;                 // 签发日，空则取今天
    QDate   expire;                 // 到期日；空则按 days 推算
    int     days = 0;               // 有效期天数（expire 为空时必填）
    int     line = 0;               // 台账里的行号，报错时指回去
};

/**
 * 一条签发结果。失败的单子也留一条，交付单上要能看到谁没签出去。
 */
struct BatchResult
{
    BatchOrder order;
    QString    file;                // 写出的授权文件（失败为空）
    QString    sig;                 // base64 签名
    QString    alg = QStringLiteral("ed25519");   // 本次用的算法，写进交付单备查
    QString    error;               // 失败原因

    bool ok() const { return error.isEmpty(); }
};

/**
 * 批量签发：一张台账 → 一批 .qglic + 一张交付单。
 *
 * 为什么要单独拎出来放 qgcore：
 *   1. 签发工具和冒烟自检要用同一份实现，否则"能签"和"自检说能签"是两件事；
 *   2. 批量失败必须隔离——第 7 行机器码抄错，不能让前 6 家和后 20 家一起没签出去。
 *
 * 台账格式（CSV，UTF-8，可带 BOM；`#` 开头为注释；表头可有可无）：
 *
 *   machine,licensee,edition,days,expire
 *   A20A-...,某某机械厂,pro,365,
 *   3F1B-...,"某某传动, 二分厂",pro,,2028-06-30
 *
 * expire 与 days 二选一，两者都给时以 expire 为准（它更明确）。
 */
class LicenseBatch
{
public:
    /**
     * 解析台账。逐行校验：合格的进返回值，不合格的写进 errors（带行号），
     * 不因为一行坏掉就整批不签。
     */
    static QVector<BatchOrder> parseLedger(const QByteArray &content, QStringList *errors);

    /**
     * 按订单逐条签发到 outDir，同时写一张交付单 manifest.csv。
     *   dryRun = true 时只算出目标文件名，不建目录、不落盘。
     *   signingSeed 为 32 字节 Ed25519 种子；留空则退回 HMAC（仅供自检与旧流程）。
     * 返回结果与订单一一对应、顺序不变。
     *
     * 交付单与授权文件一起落地：分两处写就会出现“文件已经发给客户、单子上没有”的账外授权。
     */
    static QVector<BatchResult> issueAll(const QVector<BatchOrder> &orders,
                                        const QString &outDir, bool dryRun,
                                        QStringList *errors,
                                        const QByteArray &signingSeed = QByteArray());

    /** 交付单路径：<outDir>/manifest.csv */
    static QString manifestPath(const QString &outDir);

    /**
     * 交付单 CSV（UTF-8 带 BOM，Excel 直接双击不乱码）。
     * 列：machine,licensee,edition,issued,expire,file,status,alg,sig8,error
     */
    static QByteArray manifestCsv(const QVector<BatchResult> &results);

    /** 授权文件默认文件名：qglic-<授权对象>-<到期日>.qglic（授权对象会被清洗成合法文件名） */
    static QString fileNameFor(const BatchOrder &order);

    /** 把授权对象清洗成能当文件名用的片段；清洗后为空时退回机器码前 8 位 */
    static QString safeFileStem(const QString &licensee, const QString &machine);
};
