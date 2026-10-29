#pragma once

#include "core/ISolverBackend.h"

#include <QImage>
#include <QString>

/**
 * 一段可交付的内容：参数 + 结果 + 云图 + 身份信息。
 */
struct ReportContext
{
    GearParams  params;
    SolveResult result;
    QImage      stressImage;

    QString machineCode;
    QString licensee;      // 已授权时的被授权方，试用为空
    QString licenseAlg;    // 签名算法：ed25519 / hmac；写在页脚供人工核对
    bool    licensed = false;
    bool    demo     = true;

    QString meshSummary;   // 导入几何摘要，可为空
    QString note;          // 工况备注，可为空
    QString appVersion;
};

/** 单页结论的 HTML（供预览或嵌入别处）；图片以 imageSrc 引用 */
QString buildReportHtml(const ReportContext &ctx,
                        const QString &imageSrc = QStringLiteral("quickgear:stress.png"));

/** 写出 A4 单页 PDF（QTextDocument → QPdfWriter），失败时 error 带原因 */
bool writeReportPdf(const QString &path, const ReportContext &ctx, QString *error);

/** 写出 PNG 云图 */
bool writeImagePng(const QString &path, const QImage &image, QString *error);

/** 给未授权导出的图片打斜向水印 */
QImage watermarkTrial(const QImage &image, const QString &text);

/** 一句话结论，passed 返回是否达到 1.25 的通过线 */
QString conclusionText(const SolveResult &result, bool *passed);
