#ifndef CODE_INSERTER_H
#define CODE_INSERTER_H

#include "AstAnalyzer.h"
#include <QString>
#include <QList>

struct Insertion {
    int offset;        // 插入/替换位置（字符偏移）
    QString text;      // 要插入/替换的文本
    int order;         // 插入顺序（用于排序）
    int replaceLen;    // 要替换的原始代码长度（0=纯插入，>0=替换模式）
    
    Insertion() : offset(0), order(0), replaceLen(0) {}
};

class CodeInserter {
public:
    CodeInserter();
    
    // 生成插桩后的完整代码
    QString insert(const QString& sourceCode, 
                   const QList<TrackedVariable>& variables,
                   const QList<TrackedOperation>& operations,
                   bool hasMain);
    
private:
    QList<Insertion> m_insertions;
    QList<TrackedVariable> m_variables;  // 存储变量列表用于生成正确代码
    
    // 生成各种插入文本
    QString generateTrackingCode(const TrackedVariable& var);
    QString generateSwapCode(const TrackedOperation& op);
    QString generateCompareCode(const TrackedOperation& op);
    QString generateAssignCode(const TrackedOperation& op);
    QString generateReverseCode(const TrackedOperation& op);
    QString generateFillCode(const TrackedOperation& op);
    QString generateVecResizeCode(const TrackedOperation& op);
    QString generateSortCode(const TrackedOperation& op);
    QString generateContainerCode(const TrackedOperation& op);  // push_back/pop_back/insert/erase
    QString generateCrossOpCode(const TrackedOperation& op);     // CrossCompare/CrossAssign
    QString generateWrapperMain(const QList<TrackedVariable>& variables);
    
    // 获取数据指针表达式
    QString getDataExpr(const TrackedVariable& var);
    
    // 查找变量
    const TrackedVariable* findVar(const QString& name) const;
    
    // 应用所有插入（从后往前，避免偏移）
    QString applyInsertions(const QString& sourceCode);
};

#endif // CODE_INSERTER_H
