#include "AstAnalyzer.h"
#include <QDir>
#include <QTemporaryFile>
#include <QJsonParseError>
#include <QDebug>
#include <functional>

AstAnalyzer::AstAnalyzer(QObject* parent) 
    : QObject(parent), m_hasMain(false) {}

bool AstAnalyzer::analyze(const QString& code) {
    m_variables.clear();
    m_operations.clear();
    m_hasMain = false;
    m_sourceCode = code;
    m_sourceUtf8 = code.toUtf8();  // 用于byte→char offset转换
    
    // 1. 调用Clang生成AST JSON
    QString astJson = runClangAstDump(code);
    if (astJson.isEmpty()) {
        m_lastError = "Failed to run Clang AST dump";
        return false;
    }
    
    // 2. 解析JSON
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(astJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        m_lastError = "AST JSON parse error: " + parseError.errorString();
        qDebug() << "JSON parse error at:" << parseError.offset;
        qDebug() << "Raw JSON:" << astJson.left(500);
        return false;
    }
    
    // 3. 遍历AST
    QJsonObject root = doc.object();

    // 遍历所有用户自定义 FunctionDecl 节点（main + 自定义函数如 bubbleSort）
    // 通过 range 偏移过滤：只遍历在源码范围内的函数声明，排除系统头文件函数
    if (root.contains("inner")) {
        QJsonArray topNodes = root["inner"].toArray();
        for (const QJsonValue& topNode : topNodes) {
            QJsonObject obj = topNode.toObject();
            if (obj["kind"].toString() == "FunctionDecl") {
                // 过滤：函数的 range 必须在源码范围内，排除系统头文件中的函数
                QJsonObject range = obj["range"].toObject();
                if (range.contains("begin")) {
                    int beginOff = byteToCharOffset(range["begin"].toObject()["offset"].toInt());
                    int endOff = byteToCharOffset(range["end"].toObject()["offset"].toInt());
                    if (beginOff >= 0 && beginOff < m_sourceCode.length() && endOff > 0) {
                        qDebug() << "Traversing function:" << obj["name"].toString();
                        traverseAst(obj);
                    }
                }
            }
        }
    }
    
    qDebug() << "AST Analysis complete:";
    qDebug() << "  Variables:" << m_variables.size();
    for (const auto& v : m_variables) {
        qDebug() << "    -" << v.name << "type:" << v.type 
                 << "defEndOffset:" << v.defEndOffset;
    }
    qDebug() << "  Operations:" << m_operations.size();
    for (const auto& op : m_operations) {
        qDebug() << "    - type:" << (int)op.type << "array:" << op.arrayName 
                 << "i1:" << op.index1 << "i2:" << op.index2
                 << "offset:" << op.beginOffset << "-" << op.endOffset;
    }
    qDebug() << "  Has main:" << m_hasMain;
    
    return true;
}

QString AstAnalyzer::runClangAstDump(const QString& code) {
    // 创建临时源文件
    QTemporaryFile tempFile(QDir::tempPath() + "/ast_input_XXXXXX.cpp");
    if (!tempFile.open()) {
        m_lastError = "Cannot create temp file for Clang";
        return "";
    }
    tempFile.write(code.toUtf8());
    tempFile.close();
    
    // 查找Clang
    QString clangPath = findClangPath();
    
    // 调用Clang AST dump
    QStringList arguments;
    arguments << "-Xclang" << "-ast-dump=json";
    arguments << "-fsyntax-only";
    arguments << "-std=c++17";
    arguments << tempFile.fileName();
    
    QProcess process;
    process.start(clangPath, arguments);
    process.waitForFinished(15000);  // 15秒超时
    
    if (process.exitCode() != 0) {
        m_lastError = "Clang failed: " + QString::fromUtf8(process.readAllStandardError());
        return "";
    }
    
    return QString::fromUtf8(process.readAllStandardOutput());
}

void AstAnalyzer::traverseAst(const QJsonObject& node) {
    QString kind = node["kind"].toString();
    
    if (kind == "FunctionDecl") {
        // 找到函数体的 { 位置（用于后续 ParmVarDecl 的跟踪代码插入）
        QJsonObject funcRange = node["range"].toObject();
        int funcBegin = extractCharOffset(funcRange, "begin");
        int bracePos = m_sourceCode.indexOf('{', funcBegin);
        if (bracePos >= 0) {
            m_currentFunctionBodyOffset = bracePos;
        }
        
        visitFunctionDecl(node);
        // 只遍历函数体内部的节点，不继续递归到系统头文件的声明
        if (node.contains("inner")) {
            QJsonArray inner = node["inner"].toArray();
            for (const QJsonValue& child : inner) {
                if (child.isObject()) {
                    traverseAst(child.toObject());
                }
            }
        }
        
        m_currentFunctionBodyOffset = -1;  // 离开函数后重置
        return;  // 不继续外层递归，避免进入系统头文件
    }

    if (kind == "VarDecl" || kind == "ParmVarDecl") {
        visitVarDecl(node);
    } else     if (kind == "CallExpr") {
        visitCallExpr(node);
    } else if (kind == "CXXMemberCallExpr") {
        visitMemberCallExpr(node);
    } else if (kind == "BinaryOperator" || kind == "CompoundAssignOperator") {
        visitBinaryOperator(node);
    }
    
    // 递归遍历子节点（仅在函数体内部时）
    if (node.contains("inner")) {
        QJsonArray inner = node["inner"].toArray();
        for (const QJsonValue& child : inner) {
            if (child.isObject()) {
                traverseAst(child.toObject());
            }
        }
    }
}

void AstAnalyzer::visitFunctionDecl(const QJsonObject& node) {
    QString name = node["name"].toString();
    if (name == "main") {
        m_hasMain = true;
    }
}

void AstAnalyzer::visitVarDecl(const QJsonObject& node) {
    QString name = node["name"].toString();
    if (name.isEmpty()) return;
    
    // 过滤编译器内部变量（以双下划线开头）
    // Clang 会为 range-based for、string 字面量等生成 __range1、__str 等内部变量
    if (name.startsWith("__")) {
        return;
    }
    
    // 过滤 std::string::npos 等特殊常量
    if (name == "npos") {
        return;
    }
    
    // 判断是否是函数参数
    bool isParm = (node["kind"].toString() == "ParmVarDecl");
    
    QJsonObject typeObj = node["type"].toObject();
    QString qualType = typeObj["qualType"].toString();
    
    // 过滤 qualType 中包含 npos 的声明
    if (qualType.contains("npos")) {
        return;
    }
    
    // 对于引用参数（如 vector<int>&），去掉 & 和 const 以便提取类型信息
    QString cleanType = qualType;
    if (isParm) {
        cleanType.replace(QRegularExpression("\\s*&\\s*$"), "");  // 去掉末尾的 &
        cleanType.replace(QRegularExpression("^const\\s+"), "");   // 去掉开头的 const
    }
    
    TrackedVariable var;
    var.name = name;
    var.isVector = false;
    var.isString = false;
    var.isArray = false;
    var.isParameter = isParm;
    
    // 判断类型（使用清理后的类型字符串）
    if (cleanType.contains("vector")) {
        var.isVector = true;
        var.type = cleanType;
        
        // 提取元素类型：从 vector<int, ...> 中提取 int
        QRegularExpression elemRegex(R"(vector\s*<\s*(\w+))");
        QRegularExpressionMatch match = elemRegex.match(cleanType);
        if (match.hasMatch()) {
            var.elementType = match.captured(1);
        }
        
        // 提取初始化值
        if (node.contains("inner")) {
            QJsonArray inner = node["inner"].toArray();
            for (const QJsonValue& child : inner) {
                QJsonObject childObj = child.toObject();
                if (childObj["kind"].toString() == "InitListExpr" ||
                    childObj["kind"].toString() == "CXXConstructExpr") {
                    // 从源码中提取初始化值
                    QJsonObject range = childObj["range"].toObject();
                    int begin = extractCharOffset(range, "begin");
                    int end = extractCharOffset(range, "end");
                    int tokLen = range["end"].toObject()["tokLen"].toInt();
                    var.initValues = m_sourceCode.mid(begin, end + tokLen - begin);
                    break;
                }
            }
        }
        
        // 对于函数参数，跟踪代码插入到函数体开头（{ 之后）
        if (isParm && m_currentFunctionBodyOffset >= 0) {
            var.defEndOffset = m_currentFunctionBodyOffset + 1;
        } else {
            // 在原始源码中查找变量声明语句的分号位置
            QJsonObject range = node["range"].toObject();
            int beginOff = extractCharOffset(range, "begin");
            int semiPos = m_sourceCode.indexOf(';', beginOff);
            if (semiPos > 0) {
                var.defEndOffset = semiPos + 1;
            } else {
                int endOffset = extractCharOffset(range, "end");
                int tokLen = range["end"].toObject()["tokLen"].toInt();
                var.defEndOffset = endOffset + tokLen;
            }
        }
        
        m_variables.append(var);
        
    } else if (qualType.contains("string") || qualType.contains("basic_string")) {
        var.isString = true;
        var.type = cleanType;
        var.elementType = "char";
        
        // 对于函数参数，跟踪代码插入到函数体开头（{ 之后）
        if (isParm && m_currentFunctionBodyOffset >= 0) {
            var.defEndOffset = m_currentFunctionBodyOffset + 1;
        } else {
            QJsonObject range2 = node["range"].toObject();
            int beginOff2 = extractCharOffset(range2, "begin");
            int semiPos2 = m_sourceCode.indexOf(';', beginOff2);
            if (semiPos2 > 0) {
                var.defEndOffset = semiPos2 + 1;
            } else {
                int endOffset2 = extractCharOffset(range2, "end");
                int tokLen2 = range2["end"].toObject()["tokLen"].toInt();
                var.defEndOffset = endOffset2 + tokLen2;
            }
        }
        
        m_variables.append(var);
        
    } else if (cleanType.contains("int [") || cleanType.contains("int[]") ||
               cleanType.contains("float [") || cleanType.contains("double [")) {
        var.isArray = true;
        var.type = cleanType;
        
        // 提取元素类型
        QRegularExpression elemRegex(R"((int|float|double|char)\s*\[)");
        QRegularExpressionMatch match = elemRegex.match(cleanType);
        if (match.hasMatch()) {
            var.elementType = match.captured(1);
        }
        
        // 提取初始化值
        if (node.contains("inner")) {
            QJsonArray inner = node["inner"].toArray();
            for (const QJsonValue& child : inner) {
                QJsonObject childObj = child.toObject();
                if (childObj["kind"].toString() == "InitListExpr") {
                    QJsonObject range = childObj["range"].toObject();
                    int begin = extractCharOffset(range, "begin");
                    int end = extractCharOffset(range, "end");
                    int tokLen = range["end"].toObject()["tokLen"].toInt();
                    var.initValues = m_sourceCode.mid(begin, end + tokLen - begin);
                    break;
                }
            }
        }
        
        // 对于函数参数，跟踪代码插入到函数体开头（{ 之后）
        if (isParm && m_currentFunctionBodyOffset >= 0) {
            var.defEndOffset = m_currentFunctionBodyOffset + 1;
        } else {
            // 在原始源码中查找变量声明语句的分号位置
            QJsonObject range3 = node["range"].toObject();
            int beginOff3 = extractCharOffset(range3, "begin");
            int semiPos3 = m_sourceCode.indexOf(';', beginOff3);
            if (semiPos3 > 0) {
                var.defEndOffset = semiPos3 + 1;
            } else {
                int endOffset3 = extractCharOffset(range3, "end");
                int tokLen3 = range3["end"].toObject()["tokLen"].toInt();
                var.defEndOffset = endOffset3 + tokLen3;
            }
        }
        
        m_variables.append(var);
    }
}

void AstAnalyzer::visitCallExpr(const QJsonObject& node) {
    if (!node.contains("inner")) return;
    
    QJsonArray inner = node["inner"].toArray();
    if (inner.isEmpty()) return;
    
    // 获取调用函数名
    QString calledFuncName;
    QJsonObject firstChild = inner[0].toObject();
    
    std::function<void(const QJsonObject&)> findFuncName = 
        [&](const QJsonObject& obj) {
        if (obj["kind"].toString() == "DeclRefExpr") {
            QJsonObject refDecl = obj["referencedDecl"].toObject();
            calledFuncName = refDecl["name"].toString();
        } else if (obj.contains("inner")) {
            QJsonArray childArray = obj["inner"].toArray();
            for (const QJsonValue& child : childArray) {
                findFuncName(child.toObject());
            }
        }
    };
    findFuncName(firstChild);
    
    // ==================== STL reverse 检测 ====================
    if (calledFuncName == "reverse") {
        visitStlReverse(node, inner);
        return;
    }
    
    // ==================== STL fill 检测 ====================
    if (calledFuncName == "fill") {
        visitStlFill(node, inner);
        return;
    }
    
    // ==================== STL sort 检测 ====================
    if (calledFuncName == "sort") {
        visitStlSort(node, inner);
        return;
    }
    
    // ==================== swap 检测（保持原有逻辑）====================
    if (calledFuncName != "swap") return;
    
    // swap有两个参数，检查是否都是tracked变量的下标
    if (inner.size() < 3) return;  // func + arg1 + arg2
    
    // 提取参数信息
    for (const auto& var : m_variables) {
        QString varName = var.name;
        
        // 检查参数1和参数2是否都是 varName[i] 形式
        bool arg1IsSubscript = isArraySubscriptOn(inner[1].toObject(), varName);
        bool arg2IsSubscript = isArraySubscriptOn(inner[2].toObject(), varName);
        
        if (arg1IsSubscript && arg2IsSubscript) {
            TrackedOperation op;
            op.type = TrackedOperation::Swap;
            op.arrayName = varName;
            op.index1 = getSubscriptIndex(inner[1].toObject());
            op.index2 = getSubscriptIndex(inner[2].toObject());
            
            QJsonObject range = node["range"].toObject();
            op.beginOffset = extractCharOffset(range, "begin");
            int endOffset = extractCharOffset(range, "end");
            int tokLen = range["end"].toObject()["tokLen"].toInt();
            op.endOffset = endOffset + tokLen;
            
            qDebug() << "  [Swap] array:" << op.arrayName << "i1:" << op.index1 << "i2:" << op.index2;
            m_operations.append(op);
            return;
        }
    }
}

void AstAnalyzer::visitMemberCallExpr(const QJsonObject& node) {
    // 处理 CXXMemberCallExpr（如 v.push_back(x)、v.pop_back() 等）
    // CXXMemberCallExpr 的 inner 结构：
    //   [0]: MemberExpr（方法引用，如 push_back）
    //   [1+]: 显式参数 / 隐式 this
    if (!node.contains("inner")) return;
    QJsonArray inner = node["inner"].toArray();
    if (inner.isEmpty()) return;

    // 提取方法名（从 MemberExpr 节点）
    QString memberName;
    std::function<void(const QJsonObject&)> findMemberName =
        [&](const QJsonObject& obj) {
        if (obj["kind"].toString() == "MemberExpr") {
            memberName = obj["name"].toString();
        } else if (obj.contains("inner")) {
            for (const auto& child : obj["inner"].toArray()) {
                findMemberName(child.toObject());
                if (!memberName.isEmpty()) return;
            }
        }
    };
    findMemberName(inner[0].toObject());
    if (memberName.isEmpty()) return;

    // 只处理容器操作
    static QStringList containerFuncs = {"push_back", "pop_back", "insert", "erase", "clear", "resize"};
    if (!containerFuncs.contains(memberName)) return;

    // 获取容器变量名
    QString varName = extractContainerObjectName(node);
    if (varName.isEmpty() || !isTrackedVariable(varName)) return;

    TrackedOperation op;
    if (memberName == "push_back")      op.type = TrackedOperation::StlPushBack;
    else if (memberName == "pop_back")  op.type = TrackedOperation::StlPopBack;
    else if (memberName == "insert")    op.type = TrackedOperation::StlInsert;
    else if (memberName == "erase")     op.type = TrackedOperation::StlErase;
    else if (memberName == "clear")     op.type = TrackedOperation::StlClear;
    else if (memberName == "resize")    op.type = TrackedOperation::StlVecResize;
    op.arrayName = varName;

    // 提取参数：
    // 此 Clang 版本将 "this" 对象嵌套在 MemberExpr（inner[0]）内部（如
    // DeclRefExpr(vv)），不作为独立的 inner child。因此 inner[1] 直接就是
    // 第一个显式参数（可能包裹在 ImplicitCastExpr/MaterializeTemporaryExpr 中）。
    // extractSourceExpr 会递归穿透这些包装节点提取实际表达式。
    //
    // 注意：erase/insert 的第一个参数是复杂迭代器表达式（如 vv.end()-1），
    // 目前不做自动提取，留空让 CodeInserter 用近似 fallback。
    if (memberName == "push_back") {
        // push_back(val): inner[1] = 值表达式
        if (inner.size() > 1) {
            op.index1 = extractSourceExpr(inner[1].toObject());
        }
    } else if (memberName == "insert") {
        // insert(pos, val): inner[1]=iterator(如 begin()+2), inner[2]=value
        // 也支持 insert(pos, count, val): inner[1]=pos, inner[2]=count, inner[3]=val
        if (inner.size() > 1) {
            // 尝试 AST 方式提取迭代器偏移
            op.index1 = extractIteratorOffset(inner[1].toObject(), varName);
            if (op.index1.isEmpty()) {
                // AST 穿透失败 → 从源码文本中 parsing 偏移量
                // 例如：vv.begin()+2 → 提取 "2" ; vv.begin() → "0"
                QString rawPos = extractSourceExpr(inner[1].toObject());
                if (rawPos.isEmpty() && inner.size() > 1) {
                    // extractSourceExpr 也失败 → 直接从原始源码截取
                    QJsonObject posRange = inner[1].toObject()["range"].toObject();
                    int pBegin = extractCharOffset(posRange, "begin");
                    int pEnd = extractCharOffset(posRange, "end");
                    int pTokLen = posRange["end"].toObject()["tokLen"].toInt();
                    rawPos = m_sourceCode.mid(pBegin, pEnd + pTokLen - pBegin);
                }
                if (!rawPos.isEmpty()) {
                    // 匹配 begin()/rbegin() 后跟 +N 或 -N 的模式
                    QRegularExpression re(R"((?:begin|rbegin)\(\s*\)\s*\+\s*(\S+))");
                    QRegularExpressionMatch m = re.match(rawPos);
                    if (m.hasMatch()) {
                        op.index1 = m.captured(1);  // 如 "2"
                    } else {
                        // 匹配 end()/rend() 后跟 -N 的模式
                        QRegularExpression reEnd(R"((?:end|rend)\(\s*\)\s*-\s*(\S+))");
                        QRegularExpressionMatch me = reEnd.match(rawPos);
                        if (me.hasMatch()) {
                            op.index1 = varName + ".size() - " + me.captured(1);
                        }
                    }
                }
            }
        }
        if (inner.size() > 3) {
            // insert(pos, count, val): count 存 index2, val 存 opcode
            op.index2 = extractSourceExpr(inner[2].toObject());
            op.opcode = extractSourceExpr(inner[3].toObject());
        } else if (inner.size() > 2) {
            // insert(pos, val): val 存 index2
            op.index2 = extractSourceExpr(inner[2].toObject());
        }
    } else if (memberName == "resize") {
        // resize(n) 或 resize(n, val)
        // inner[1] = 新大小表达式, inner[2](可选) = 填充值
        if (inner.size() > 1) {
            op.index1 = extractSourceExpr(inner[1].toObject());  // 新大小
        }
        if (inner.size() > 2) {
            op.index2 = extractSourceExpr(inner[2].toObject());  // 填充值（可选）
        }
    } else if (memberName == "erase") {
        // erase(pos) 或 erase(first, last)
        // 使用与 insert 相同的迭代器偏移提取逻辑
        if (inner.size() > 1) {
            op.index1 = extractIteratorOffset(inner[1].toObject(), varName);
            if (op.index1.isEmpty()) {
                QString rawPos = extractSourceExpr(inner[1].toObject());
                if (rawPos.isEmpty()) {
                    QJsonObject posRange = inner[1].toObject()["range"].toObject();
                    int pBegin = extractCharOffset(posRange, "begin");
                    int pEnd = extractCharOffset(posRange, "end");
                    int pTokLen = posRange["end"].toObject()["tokLen"].toInt();
                    rawPos = m_sourceCode.mid(pBegin, pEnd + pTokLen - pBegin);
                }
                if (!rawPos.isEmpty()) {
                    QRegularExpression re(R"((?:begin|rbegin)\(\s*\)\s*\+\s*(\S+))");
                    QRegularExpressionMatch m = re.match(rawPos);
                    if (m.hasMatch()) {
                        op.index1 = m.captured(1);
                    } else {
                        QRegularExpression reEnd(R"((?:end|rend)\(\s*\)\s*-\s*(\S+))");
                        QRegularExpressionMatch me = reEnd.match(rawPos);
                        if (me.hasMatch()) {
                            op.index1 = varName + ".size() - " + me.captured(1);
                        }
                    }
                }
            }
        }
        // erase(first, last): 提取第二个迭代器参数
        if (inner.size() > 2) {
            op.index2 = extractIteratorOffset(inner[2].toObject(), varName);
            if (op.index2.isEmpty()) {
                QString rawPos = extractSourceExpr(inner[2].toObject());
                if (rawPos.isEmpty()) {
                    QJsonObject posRange = inner[2].toObject()["range"].toObject();
                    int pBegin = extractCharOffset(posRange, "begin");
                    int pEnd = extractCharOffset(posRange, "end");
                    int pTokLen = posRange["end"].toObject()["tokLen"].toInt();
                    rawPos = m_sourceCode.mid(pBegin, pEnd + pTokLen - pBegin);
                }
                if (!rawPos.isEmpty()) {
                    QRegularExpression re(R"((?:begin|rbegin)\(\s*\)\s*\+\s*(\S+))");
                    QRegularExpressionMatch m = re.match(rawPos);
                    if (m.hasMatch()) {
                        op.index2 = m.captured(1);
                    } else {
                        QRegularExpression reEnd(R"((?:end|rend)\(\s*\)\s*-\s*(\S+))");
                        QRegularExpressionMatch me = reEnd.match(rawPos);
                        if (me.hasMatch()) {
                            op.index2 = varName + ".size() - " + me.captured(1);
                        }
                    }
                }
            }
        }
    }

    QJsonObject range = node["range"].toObject();
    op.beginOffset = extractCharOffset(range, "begin");
    int endOffset = extractCharOffset(range, "end");
    int tokLen = range["end"].toObject()["tokLen"].toInt();
    op.endOffset = endOffset + tokLen;

    qDebug() << "  [STL Container]" << memberName << "array:" << op.arrayName
             << "idx1:" << op.index1 << "idx2:" << op.index2;
    m_operations.append(op);
}

void AstAnalyzer::visitStlReverse(const QJsonObject& node, const QJsonArray& inner) {
    // reverse 需要 2 个参数（func + arg1 + arg2）
    if (inner.size() < 3) return;
    
    // 检查两个参数是否都是已追踪 vector 的 .begin() / .end() 调用
    // 支持形式：reverse(arr.begin(), arr.end()) / reverse(arr.begin() + 1, arr.end() - 1)
    
    for (const auto& var : m_variables) {
        if (!var.isVector && !var.isArray) continue;
        
        // 检查参数1是否引用了该变量
        QString arg1Name = getReferencedName(inner[1].toObject());
        if (arg1Name.isEmpty()) {
            // 可能在 CXXMemberCallExpr 内部
            arg1Name = extractVectorNameFromIterator(inner[1].toObject());
        }
        
        // 检查参数2
        QString arg2Name = getReferencedName(inner[2].toObject());
        if (arg2Name.isEmpty()) {
            arg2Name = extractVectorNameFromIterator(inner[2].toObject());
        }
        
        if (arg1Name != var.name || arg2Name != var.name) continue;
        
        // 确认参数确实是 begin()/end() 相关调用
        if (!isIteratorCall(inner[1].toObject()) || !isIteratorCall(inner[2].toObject()))
            continue;
        
        TrackedOperation op;
        op.type = TrackedOperation::StlReverse;
        op.arrayName = var.name;
        
        // 提取迭代器偏移表达式
        op.index1 = extractIteratorOffset(inner[1].toObject(), var.name);  // begin 偏移
        op.index2 = extractIteratorOffset(inner[2].toObject(), var.name);  // end 偏移
        
        QJsonObject range = node["range"].toObject();
        op.beginOffset = extractCharOffset(range, "begin");
        int endOffset = extractCharOffset(range, "end");
        int tokLen = range["end"].toObject()["tokLen"].toInt();
        op.endOffset = endOffset + tokLen;
        
        qDebug() << "  [StlReverse] array:" << op.arrayName
                 << "beginOff:" << op.index1 << "endOff:" << op.index2;
        m_operations.append(op);
        return;
    }
}

void AstAnalyzer::visitStlFill(const QJsonObject& node, const QJsonArray& inner) {
    // fill(begin, end, value) 需要 4 个 inner 节点：函数引用 + 3 个参数
    // inner[0]: DeclRefExpr/ImplicitCastExpr(fill)
    // inner[1]: begin 迭代器参数
    // inner[2]: end 迭代器参数
    // inner[3]: 填充值表达式
    if (inner.size() < 4) return;

    // 检查两个迭代器参数是否引用了同一个已追踪变量
    for (const auto& var : m_variables) {
        if (!var.isVector && !var.isArray) continue;

        // 提取参数1(begin)和参数2(end)中的变量名
        QString arg1Name = getReferencedName(inner[1].toObject());
        if (arg1Name.isEmpty())
            arg1Name = extractVectorNameFromIterator(inner[1].toObject());

        QString arg2Name = getReferencedName(inner[2].toObject());
        if (arg2Name.isEmpty())
            arg2Name = extractVectorNameFromIterator(inner[2].toObject());

        if (arg1Name != var.name || arg2Name != var.name) continue;

        // 至少 begin 参数需是迭代器调用
        if (!isIteratorCall(inner[1].toObject()) && !isIteratorCall(inner[2].toObject()))
            continue;

        TrackedOperation op;
        op.type = TrackedOperation::StlFill;
        op.arrayName = var.name;

        // 提取 begin/end 偏移表达式（同 reverse）
        op.index1 = extractIteratorOffset(inner[1].toObject(), var.name);  // begin 偏移
        op.index2 = extractIteratorOffset(inner[2].toObject(), var.name);  // end 偏移

        // 提取第三个参数（填充值），穿透包装节点
        op.opcode = extractSourceExpr(inner[3].toObject());

        QJsonObject range = node["range"].toObject();
        op.beginOffset = extractCharOffset(range, "begin");
        int endOffset = extractCharOffset(range, "end");
        int tokLen = range["end"].toObject()["tokLen"].toInt();
        op.endOffset = endOffset + tokLen;

        qDebug() << "  [StlFill] array:" << op.arrayName
                 << "beginOff:" << op.index1 << "endOff:" << op.index2
                 << "val:" << op.opcode;
        m_operations.append(op);
        return;
    }
}

void AstAnalyzer::visitStlSort(const QJsonObject& node, const QJsonArray& inner) {
    // sort(begin, end) 或 sort(begin, end, comparator)
    // inner[0]: 函数引用
    // inner[1]: begin 迭代器
    // inner[2]: end 迭代器
    // inner[3] (可选): 比较器 (如 greater<int>())
    if (inner.size() < 3) return;

    // 检查两个迭代器参数是否引用了同一个已追踪变量
    for (const auto& var : m_variables) {
        if (!var.isVector && !var.isArray) continue;

        // 提取参数1(begin)和参数2(end)中的变量名
        QString arg1Name = getReferencedName(inner[1].toObject());
        if (arg1Name.isEmpty())
            arg1Name = extractVectorNameFromIterator(inner[1].toObject());

        QString arg2Name = getReferencedName(inner[2].toObject());
        if (arg2Name.isEmpty())
            arg2Name = extractVectorNameFromIterator(inner[2].toObject());

        if (arg1Name != var.name || arg2Name != var.name) continue;

        // 至少 begin 参数需是迭代器调用
        if (!isIteratorCall(inner[1].toObject()) && !isIteratorCall(inner[2].toObject()))
            continue;

        TrackedOperation op;
        op.type = TrackedOperation::StlSort;
        op.arrayName = var.name;

        // 提取 begin/end 偏移表达式
        op.index1 = extractIteratorOffset(inner[1].toObject(), var.name);  // begin 偏移
        op.index2 = extractIteratorOffset(inner[2].toObject(), var.name);  // end 偏移

        // 提取比较器（第三个参数，可选）
        // 识别：greater<int>() → 降序, less<int>() → 升序, 省略 → 升序
        if (inner.size() > 3) {
            // 提取比较器源码文本（如 "greater<int>()", "less<int>()", lambda 等）
            op.opcode = extractSourceExpr(inner[3].toObject());
            if (op.opcode.isEmpty()) {
                // 递归穿透包装节点尝试提取
                op.opcode = extractSourceExpr(inner[3].toObject());
            }
        }
        // 如果 opcode 为空，默认为升序（less）

        QJsonObject range = node["range"].toObject();
        op.beginOffset = extractCharOffset(range, "begin");
        int endOffset = extractCharOffset(range, "end");
        int tokLen = range["end"].toObject()["tokLen"].toInt();
        op.endOffset = endOffset + tokLen;

        qDebug() << "  [StlSort] array:" << op.arrayName
                 << "beginOff:" << op.index1 << "endOff:" << op.index2
                 << "cmp:" << op.opcode;
        m_operations.append(op);
        return;
    }
}

// 辅助：从迭代器表达式（如 arr.begin(), arr.begin()+1）提取 vector 名
QString AstAnalyzer::extractVectorNameFromIterator(const QJsonObject& node) {
    // 递归查找 DeclRefExpr
    QString name = getReferencedName(node);
    if (!name.isEmpty()) return name;
    
    if (!node.contains("inner")) return "";
    QJsonArray inner = node["inner"].toArray();
    for (const QJsonValue& child : inner) {
        QString n = extractVectorNameFromIterator(child.toObject());
        if (!n.isEmpty()) return n;
    }
    return "";
}

// 辅助：检查节点是否是迭代器调用（.begin() / .end() / .rbegin() / .rend()）
bool AstAnalyzer::isIteratorCall(const QJsonObject& node) {
    QString kind = node["kind"].toString();
    
    // CXXMemberCallExpr: arr.begin()
    if (kind == "CXXMemberCallExpr") {
        // 查找 MemberExpr
        if (!node.contains("inner")) return false;
        QJsonArray inner = node["inner"].toArray();
        for (const QJsonValue& child : inner) {
            QJsonObject childObj = child.toObject();
            if (childObj["kind"].toString() == "MemberExpr") {
                QString memberName = childObj["name"].toString();
                return memberName.contains("begin") || memberName.contains("end") ||
                       memberName.contains("rbegin") || memberName.contains("rend");
            }
        }
        return false;
    }
    
    // BinaryOperator: arr.begin() + 1
    if (kind == "BinaryOperator") {
        if (!node.contains("inner")) return false;
        QJsonArray inner = node["inner"].toArray();
        if (inner.size() >= 2) {
            return isIteratorCall(inner[0].toObject()) || isIteratorCall(inner[1].toObject());
        }
        return false;
    }
    
    // UnaryOperator: arr.end() - 1（clang可能解析为 +(-1)）
    if (kind == "UnaryOperator") {
        if (!node.contains("inner")) return false;
        QJsonArray inner = node["inner"].toArray();
        if (!inner.isEmpty()) {
            return isIteratorCall(inner[0].toObject());
        }
        return false;
    }
    
    // ImplicitCastExpr: 包装迭代器
    if (kind == "ImplicitCastExpr" && node.contains("inner")) {
        QJsonArray inner = node["inner"].toArray();
        if (!inner.isEmpty()) {
            return isIteratorCall(inner[0].toObject());
        }
    }
    
    return false;
}

// 辅助：从迭代器表达式提取索引偏移字符串
// arr.begin()      → "0"
// arr.begin() + 1  → "1"
// arr.end()        → "arr.size()"   (因为 end-1 会由调用方处理)
// arr.end() - 1    → "arr.size() - 1"
QString AstAnalyzer::extractIteratorOffset(const QJsonObject& node, const QString& varName) {
    QString kind = node["kind"].toString();
    
    // 直接迭代器调用：arr.begin() / arr.end()
    if (kind == "CXXMemberCallExpr") {
        if (!node.contains("inner")) return "";
        QJsonArray inner = node["inner"].toArray();
        for (const QJsonValue& child : inner) {
            QJsonObject childObj = child.toObject();
            if (childObj["kind"].toString() == "MemberExpr") {
                QString memberName = childObj["name"].toString();
                if (memberName.contains("begin")) {
                    return "0";
                } else if (memberName.contains("end")) {
                    return varName + ".size()";
                }
            }
        }
        return "";
    }
    
    // BinaryOperator: arr.begin() + n  或 arr.end() - n
    if (kind == "BinaryOperator") {
        if (!node.contains("inner")) return "";
        QJsonArray inner = node["inner"].toArray();
        if (inner.size() < 2) return "";
        
        QJsonObject lhs = inner[0].toObject();
        QJsonObject rhs = inner[1].toObject();
        QString opcode = node["opcode"].toString();
        
        // 检查 lhs 是否是迭代器调用
        QString baseOffset = extractIteratorOffset(lhs, varName);
        if (baseOffset.isEmpty()) {
            // 可能是 rhs
            baseOffset = extractIteratorOffset(rhs, varName);
            if (!baseOffset.isEmpty()) {
                // 交换 lhs/rhs
                QJsonObject tmp = lhs; lhs = rhs; rhs = tmp;
            }
        }
        if (baseOffset.isEmpty()) return "";
        
        // 提取 rhs 的常量表达式
        QString rhsExpr = extractSourceExpr(rhs);
        if (rhsExpr.isEmpty()) return "";
        
        if (opcode == "+") {
            if (baseOffset == "0") return rhsExpr;
            return baseOffset + " + " + rhsExpr;
        } else if (opcode == "-") {
            if (baseOffset == "0") return "-" + rhsExpr;
            return baseOffset + " - " + rhsExpr;
        }
        return "";
    }
    
    // ImplicitCastExpr: 包装
    if (kind == "ImplicitCastExpr" && node.contains("inner")) {
        QJsonArray inner = node["inner"].toArray();
        if (!inner.isEmpty()) {
            return extractIteratorOffset(inner[0].toObject(), varName);
        }
    }
    
    return "";
}

// 辅助：从 AST 节点提取源码表达式文本
QString AstAnalyzer::extractSourceExpr(const QJsonObject& node) {
    if (node.isEmpty()) return "";
    QJsonObject range = node["range"].toObject();
    if (!range.isEmpty()) {
        int begin = extractCharOffset(range, "begin");
        int end = extractCharOffset(range, "end");
        int tokLen = range["end"].toObject()["tokLen"].toInt();
        QString result = m_sourceCode.mid(begin, end + tokLen - begin);
        if (!result.isEmpty()) return result;
    }
    // 节点无有效 range（如 MaterializeTemporaryExpr）或提取为空，
    // 递归穿透 inner 子节点查找有 range 的实际表达式
    if (node.contains("inner")) {
        QJsonArray inner = node["inner"].toArray();
        for (const auto& child : inner) {
            QString expr = extractSourceExpr(child.toObject());
            if (!expr.isEmpty()) return expr;
        }
    }
    return "";
}

void AstAnalyzer::visitBinaryOperator(const QJsonObject& node) {
    QString opcode = node["opcode"].toString();
    if (opcode.isEmpty()) return;
    
    if (!node.contains("inner") || node["inner"].toArray().size() < 2) return;
    
    QJsonArray inner = node["inner"].toArray();
    QJsonObject lhs = inner[0].toObject();
    QJsonObject rhs = inner[1].toObject();
    
    // 检查比较操作：v[i] > v[j]  或 跨数组 arr[i] > brr[j]
    if (opcode == ">" || opcode == "<" || opcode == ">=" || 
        opcode == "<=" || opcode == "==" || opcode == "!=") {
        
        for (const auto& var : m_variables) {
            QString varName = var.name;
            
            if (isArraySubscriptOn(lhs, varName) && isArraySubscriptOn(rhs, varName)) {
                // 同数组比较
                TrackedOperation op;
                op.type = TrackedOperation::Compare;
                op.arrayName = varName;
                op.opcode = opcode;
                op.index1 = getSubscriptIndex(lhs);
                op.index2 = getSubscriptIndex(rhs);
                
                QJsonObject range = node["range"].toObject();
                op.beginOffset = extractCharOffset(range, "begin");
                int endOffset = extractCharOffset(range, "end");
                int tokLen = range["end"].toObject()["tokLen"].toInt();
                op.endOffset = endOffset + tokLen;
                
                qDebug() << "  [Compare] array:" << op.arrayName << "op:" << op.opcode << "i1:" << op.index1 << "i2:" << op.index2;
                m_operations.append(op);
                return;
            }
        }

        // 跨数组比较：检查 LHS 和 RHS 是否属于不同的已追踪变量
        for (const auto& varA : m_variables) {
            if (!isArraySubscriptOn(lhs, varA.name)) continue;
            for (const auto& varB : m_variables) {
                if (varA.name == varB.name) continue;
                if (!isArraySubscriptOn(rhs, varB.name)) continue;

                TrackedOperation op;
                op.type = TrackedOperation::CrossCompare;
                op.arrayName = varA.name;
                op.index1 = getSubscriptIndex(lhs);
                op.otherArrayName = varB.name;
                op.otherIndex = getSubscriptIndex(rhs);
                op.opcode = opcode;

                QJsonObject range = node["range"].toObject();
                op.beginOffset = extractCharOffset(range, "begin");
                int endOffset = extractCharOffset(range, "end");
                int tokLen = range["end"].toObject()["tokLen"].toInt();
                op.endOffset = endOffset + tokLen;

                qDebug() << "  [CrossCompare]" << varA.name << "[" << op.index1 << "]"
                         << op.opcode << varB.name << "[" << op.otherIndex << "]";
                m_operations.append(op);
                return;
            }
        }
    }
    
    // 检查赋值操作：v[i] = v[j]  或  跨数组 arr[i] = brr[j]
    if (opcode == "=") {
        for (const auto& var : m_variables) {
            QString varName = var.name;
            
            if (isArraySubscriptOn(lhs, varName)) {
                TrackedOperation op;
                op.arrayName = varName;
                op.index1 = getSubscriptIndex(lhs);
                
                QJsonObject range = node["range"].toObject();
                op.beginOffset = extractCharOffset(range, "begin");
                int endOffset = extractCharOffset(range, "end");
                int tokLen = range["end"].toObject()["tokLen"].toInt();
                op.endOffset = endOffset + tokLen;
                
                if (isArraySubscriptOn(rhs, varName)) {
                    op.type = TrackedOperation::ArrayAssign;
                    op.index2 = getSubscriptIndex(rhs);
                } else {
                    // 检查 RHS 是否是对另一个已追踪变量的下标
                    bool crossFound = false;
                    for (const auto& varB : m_variables) {
                        if (varB.name == varName) continue;
                        if (isArraySubscriptOn(rhs, varB.name)) {
                            op.type = TrackedOperation::CrossAssign;
                            op.otherArrayName = varB.name;
                            op.otherIndex = getSubscriptIndex(rhs);
                            crossFound = true;
                            break;
                        }
                    }
                    if (!crossFound) {
                        op.type = TrackedOperation::ValueAssign;
                        // 提取 RHS 表达式源码文本（如 vv[i] = i 中的 "i"）
                        QJsonObject rhsRange = rhs["range"].toObject();
                        int rhsBegin = extractCharOffset(rhsRange, "begin");
                        int rhsEnd = extractCharOffset(rhsRange, "end");
                        int rhsTokLen = rhsRange["end"].toObject()["tokLen"].toInt();
                        op.index2 = m_sourceCode.mid(rhsBegin, rhsEnd + rhsTokLen - rhsBegin);
                    }
                }
                
                qDebug() << "  [Assign] type:" << (int)op.type << "array:" << op.arrayName 
                         << "i1:" << op.index1 << "i2:" << op.index2
                         << "other:" << op.otherArrayName << "[" << op.otherIndex << "]";
                m_operations.append(op);
                return;
            }
        }
    }
}

QString AstAnalyzer::getReferencedName(const QJsonObject& node) {
    if (node["kind"].toString() == "DeclRefExpr") {
        QJsonObject refDecl = node["referencedDecl"].toObject();
        return refDecl["name"].toString();
    }
    if (node.contains("inner")) {
        QJsonArray inner = node["inner"].toArray();
        for (const QJsonValue& child : inner) {
            QString name = getReferencedName(child.toObject());
            if (!name.isEmpty()) return name;
        }
    }
    return "";
}

bool AstAnalyzer::isArraySubscriptOn(const QJsonObject& node, const QString& varName) {
    // 处理原生数组的下标访问：a[i]
    if (node["kind"].toString() == "ArraySubscriptExpr") {
        if (!node.contains("inner")) return false;
        QJsonArray inner = node["inner"].toArray();
        if (inner.isEmpty()) return false;

        QString baseName = getReferencedName(inner[0].toObject());
        return baseName == varName;
    }

    // 处理 vector 的下标访问：v[i]（通过 operator[]）
    if (node["kind"].toString() == "CXXOperatorCallExpr") {
        // 检查是否是 operator[] 调用
        if (!node.contains("inner")) return false;
        QJsonArray inner = node["inner"].toArray();
        if (inner.size() < 2) return false;  // 需要 [函数引用, 对象, 索引]

        // 第一个子节点是 operator[] 的引用
        QJsonObject funcRef = inner[0].toObject();
        QString funcName;
        // 递归查找函数名
        std::function<void(const QJsonObject&)> findOpName = [&](const QJsonObject& obj) {
            if (obj["kind"].toString() == "DeclRefExpr") {
                QJsonObject refDecl = obj["referencedDecl"].toObject();
                funcName = refDecl["name"].toString();
            } else if (obj.contains("inner")) {
                for (const auto& child : obj["inner"].toArray()) {
                    findOpName(child.toObject());
                }
            }
        };
        findOpName(funcRef);

        // 检查是否是 operator[]
        if (!funcName.contains("operator[]") && !funcName.contains("operator[]=")) {
            return false;
        }

        // 第二个子节点是被操作的对象（vector 本身）
        QString baseName = getReferencedName(inner[1].toObject());
        return baseName == varName;
    }

    // 处理隐式转换
    if (node["kind"].toString() == "ImplicitCastExpr" && node.contains("inner")) {
        QJsonArray inner = node["inner"].toArray();
        if (!inner.isEmpty()) {
            return isArraySubscriptOn(inner[0].toObject(), varName);
        }
    }
    
    return false;
}

QString AstAnalyzer::getSubscriptIndex(const QJsonObject& node) {
    QJsonObject subscriptNode;
    
    if (node["kind"].toString() == "ArraySubscriptExpr") {
        subscriptNode = node;
    } 
    // 处理 vector 的下标访问
    else if (node["kind"].toString() == "CXXOperatorCallExpr") {
        subscriptNode = node;
    }
    else if (node["kind"].toString() == "ImplicitCastExpr" && node.contains("inner")) {
        QJsonArray inner = node["inner"].toArray();
        for (const QJsonValue& child : inner) {
            QString kind = child.toObject()["kind"].toString();
            if (kind == "ArraySubscriptExpr" || kind == "CXXOperatorCallExpr") {
                subscriptNode = child.toObject();
                break;
            }
        }
    }

    if (subscriptNode.isEmpty() || !subscriptNode.contains("inner")) return "";

    QJsonArray inner = subscriptNode["inner"].toArray();
    
    // 对于 ArraySubscriptExpr: inner = [base, index]
    // 对于 CXXOperatorCallExpr: inner = [operator[], base, index]
    int indexPos;
    if (subscriptNode["kind"].toString() == "CXXOperatorCallExpr") {
        if (inner.size() < 3) return "";
        indexPos = 2;  // 第三个元素是索引
    } else {
        if (inner.size() < 2) return "";
        indexPos = 1;  // 第二个元素是索引
    }

    QJsonObject indexNode = inner[indexPos].toObject();

    // 获取索引的源码文本（转换为char offset）
    QJsonObject range = indexNode["range"].toObject();
    int begin = extractCharOffset(range, "begin");
    int end = extractCharOffset(range, "end");
    int tokLen = range["end"].toObject()["tokLen"].toInt();

    return m_sourceCode.mid(begin, end + tokLen - begin);
}

bool AstAnalyzer::isTrackedVariable(const QString& name) const {
    for (const auto& var : m_variables) {
        if (var.name == name) return true;
    }
    return false;
}

TrackedVariable* AstAnalyzer::findVariable(const QString& name) {
    for (auto& var : m_variables) {
        if (var.name == name) return &var;
    }
    return nullptr;
}

int AstAnalyzer::offsetToLine(int offset) const {
    int line = 1;
    for (int i = 0; i < offset && i < m_sourceCode.length(); ++i) {
        if (m_sourceCode[i] == '\n') line++;
    }
    return line;
}

int AstAnalyzer::offsetToCol(int offset) const {
    int col = 1;
    for (int i = offset - 1; i >= 0; --i) {
        if (m_sourceCode[i] == '\n') break;
        col++;
    }
    return col;
}

int AstAnalyzer::byteToCharOffset(int byteOffset) const {
    if (byteOffset <= 0) return 0;
    if (byteOffset >= m_sourceUtf8.size()) return m_sourceCode.length();
    // 将UTF-8字节前缀解码为QString，其长度即为character offset
    return QString::fromUtf8(m_sourceUtf8.left(byteOffset)).length();
}

int AstAnalyzer::extractCharOffset(const QJsonObject& rangeObj, const QString& field) const {
    QJsonObject locObj = rangeObj[field].toObject();
    if (locObj.isEmpty()) return 0;
    int byteOff = locObj["offset"].toInt();
    return byteToCharOffset(byteOff);
}

QString AstAnalyzer::findClangPath() {
    // 1. 检查VS自带的Clang
    QStringList vsPaths = {
        "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin/clang.exe",
        "C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/x64/bin/clang.exe",
        "C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/Llvm/x64/bin/clang.exe",
        "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang.exe",
        "C:/Program Files/Microsoft Visual Studio/2026/Community/VC/Tools/Llvm/x64/bin/clang.exe",
        "C:/Program Files/Microsoft Visual Studio/2026/Professional/VC/Tools/Llvm/x64/bin/clang.exe",
    };
    for (const auto& path : vsPaths) {
        if (QFile::exists(path)) return path;
    }
    
    // 2. 检查MSYS2/MinGW
    QStringList mingwPaths = {
        "C:/msys64/mingw64/bin/clang.exe",
        "C:/msys64/ucrt64/bin/clang.exe",
        "C:/msys64/clang64/bin/clang.exe",
        "C:/mingw64/bin/clang.exe",
    };
    for (const auto& path : mingwPaths) {
        if (QFile::exists(path)) return path;
    }
    
    // 3. 系统PATH
    return "clang";
}

// 辅助：从容器成员函数调用（如 v.push_back(x)）提取容器变量名
QString AstAnalyzer::extractContainerObjectName(const QJsonObject& node) {
    // v.push_back(x) 的 AST 结构：
    // CXXMemberCallExpr
    //   |- MemberExpr <-- name = "push_back"
    //   |- CXXThisExpr (隐式)
    //   |- ImplicitCastExpr
    //   |   `- DeclRefExpr <-- 这里才是 v（容器变量名）
    //   `- ... (参数)
    
    if (node["kind"].toString() != "CXXMemberCallExpr") return "";
    if (!node.contains("inner")) return "";
    
    QJsonArray inner = node["inner"].toArray();
    if (inner.isEmpty()) return "";
    
    // 遍历子节点，找 DeclRefExpr
    std::function<QString(const QJsonObject&)> findObjName =
        [&](const QJsonObject& obj) -> QString {
        if (obj["kind"].toString() == "DeclRefExpr") {
            QJsonObject refDecl = obj["referencedDecl"].toObject();
            return refDecl["name"].toString();
        }
        if (obj.contains("inner")) {
            QJsonArray childArr = obj["inner"].toArray();
            for (const QJsonValue& v : childArr) {
                QString n = findObjName(v.toObject());
                if (!n.isEmpty()) return n;
            }
        }
        return "";
    };
    
    return findObjName(inner[0].toObject());
}
