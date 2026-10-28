#pragma once

#include "ISolverBackend.h"

#include <QString>

/**
 * 一份工程（.qgpj）的全部内容：参数 + 结果摘要 + 几何引用。
 *
 * 刻意只存"能复现"的东西：几何存路径而不是塞进文件，避免体积失控；
 * 结果摘要存下来是为了重新打开时不必再跑一次就能看到上次的结论。
 */
struct ProjectData
{
    GearParams  params;
    bool        hasResult = false;
    SolveResult result;
    QString     meshPath;   // 导入的 STL 绝对路径，可为空
    QString     note;       // 用户备注（工况说明）
    QString     savedAt;    // ISO 8601
};

/** QFileDialog 用的过滤器串 */
QString projectFileFilter();

/** 原子写入（QSaveFile），失败时 error 带原因 */
bool saveProject(const QString &path, const ProjectData &data, QString *error);

/** 读取工程；文件损坏 / 版本不认识都返回 false 并给出可读原因 */
bool loadProject(const QString &path, ProjectData *data, QString *error);
