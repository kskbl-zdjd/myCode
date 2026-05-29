#include "CodeInserter.h"
#include <algorithm>
#include <QRegularExpression>

CodeInserter::CodeInserter() {}

QString CodeInserter::insert(const QString& sourceCode,
                             const QList<TrackedVariable>& variables,
                             const QList<TrackedOperation>& operations,
                             bool hasMain) {
    m_insertions.clear();
    m_variables = variables;  // 存储变量列表供后续使用
    
    // 1. 在每个变量定义后插入跟踪代码
    for (int i = 0; i < variables.size(); ++i) {
        const auto& var = variables[i];
        Insertion ins;
        ins.offset = var.defEndOffset;
        ins.text = "\n    " + generateTrackingCode(var);
        ins.order = i;  // 按变量定义顺序
        m_insertions.append(ins);
    }
    
    // 2. 替换操作为viz调用（从后往前替换，避免偏移）
    QList<TrackedOperation> sortedOps = operations;
    std::sort(sortedOps.begin(), sortedOps.end(), 
        [](const TrackedOperation& a, const TrackedOperation& b) {
            return a.beginOffset > b.beginOffset;  // 降序
        });
    
    for (const auto& op : sortedOps) {
        Insertion ins;
        ins.offset = op.beginOffset;
        ins.order = 1000 + op.beginOffset;
        
        // 设置替换长度：用viz调用替换原有操作代码
        ins.replaceLen = op.endOffset - op.beginOffset;
        
        switch (op.type) {
        case TrackedOperation::Swap:
            ins.text = generateSwapCode(op);
            break;
        case TrackedOperation::Compare:
            ins.text = generateCompareCode(op);
            break;
        case TrackedOperation::ArrayAssign:
            ins.text = generateAssignCode(op);
            break;
        case TrackedOperation::ValueAssign:
            ins.text = generateAssignCode(op);
            break;
        case TrackedOperation::StlReverse:
            ins.text = generateReverseCode(op);
            break;
        case TrackedOperation::StlFill:
            ins.text = generateFillCode(op);
            break;
        case TrackedOperation::StlVecResize:
            ins.text = generateVecResizeCode(op);
            break;
        case TrackedOperation::StlSort:
            ins.text = generateSortCode(op);
            break;
        case TrackedOperation::StlPushBack:
        case TrackedOperation::StlPopBack:
        case TrackedOperation::StlInsert:
        case TrackedOperation::StlErase:
        case TrackedOperation::StlClear:
            ins.text = generateContainerCode(op);
            break;

        case TrackedOperation::CrossCompare:
        case TrackedOperation::CrossAssign:
            ins.text = generateCrossOpCode(op);
            break;

        default:
            continue;
        }
        
        m_insertions.append(ins);
    }
    
    // 3. 添加头文件
    Insertion headerIns;
    headerIns.offset = 0;
    headerIns.text = "#include \"viz_api.h\"\n";
    headerIns.order = -1;  // 最先插入
    m_insertions.append(headerIns);
    
    // 4. 如果有main函数，生成包装；否则生成DLL入口+测试数据
    if (hasMain) {
        Insertion dllIns;
        dllIns.offset = sourceCode.length();
        dllIns.text = "\n\n"
            "extern \"C\" __declspec(dllexport) int run_algorithm(std::vector<VizEvent>* buffer) {\n"
            "    viz::setEventBuffer(buffer);\n"
            "    return main();\n"
            "}\n";
        dllIns.order = 9999;
        m_insertions.append(dllIns);
    } else {
        // 算法函数模式：生成包装main
        QString wrapperMain = generateWrapperMain(variables);
        Insertion wrapperIns;
        wrapperIns.offset = sourceCode.length();
        wrapperIns.text = "\n\n" + wrapperMain;
        wrapperIns.order = 9999;
        m_insertions.append(wrapperIns);
    }
    
    return applyInsertions(sourceCode);
}

QString CodeInserter::generateTrackingCode(const TrackedVariable& var) {
    if (var.isVector) {
        return QString("viz::array(%1, \"%1\");").arg(var.name);
    } else if (var.isArray) {
        // 从类型中提取大小，如 "int [7]" -> "7"
        QRegularExpression sizeRegex(R"(\[\s*(\d+)\s*\])");
        QRegularExpressionMatch match = sizeRegex.match(var.type);
        QString sizeExpr = match.hasMatch() ? match.captured(1) : "7";
        return QString("viz::array(%1, %2, \"%1\");").arg(var.name).arg(sizeExpr);
    } else if (var.isString) {
        return QString("viz::string(%1.c_str(), %1.size(), \"%1\");").arg(var.name);
    }
    return "";
}

QString CodeInserter::generateSwapCode(const TrackedOperation& op) {
    // 向量使用 .data()，原生数组直接使用名称
    const TrackedVariable* var = findVar(op.arrayName);
    QString dataExpr = var ? getDataExpr(*var) : op.arrayName;
    return QString("viz::swap(%1, %2, %3, \"%4\")")
        .arg(dataExpr).arg(op.index1).arg(op.index2).arg(op.arrayName);
}

QString CodeInserter::generateCompareCode(const TrackedOperation& op) {
    const TrackedVariable* var = findVar(op.arrayName);
    QString dataExpr = var ? getDataExpr(*var) : op.arrayName;
    return QString("(viz::compare(%1, %2, %3, \"%4\") %5 0)")
        .arg(dataExpr).arg(op.index1).arg(op.index2).arg(op.arrayName).arg(op.opcode);
}

QString CodeInserter::generateAssignCode(const TrackedOperation& op) {
    const TrackedVariable* var = findVar(op.arrayName);
    QString dataExpr = var ? getDataExpr(*var) : op.arrayName;
    if (op.type == TrackedOperation::ArrayAssign) {
        return QString("viz::swap(%1, %2, %3, \"%4\")")
            .arg(dataExpr).arg(op.index1).arg(op.index2).arg(op.arrayName);
    }
    if (op.type == TrackedOperation::ValueAssign) {
        return QString("viz::setValue(%1, %2, %3, \"%4\")")
            .arg(dataExpr).arg(op.index1).arg(op.index2).arg(op.arrayName);
    }
    if (op.type == TrackedOperation::CrossAssign) {
        // 跨数组赋值: arr[i] = brr[j]
        // 先读取 brr[j] 的值，再写入 arr[i]
        const TrackedVariable* otherVar = findVar(op.otherArrayName);
        QString otherDataExpr = otherVar ? getDataExpr(*otherVar) : op.otherArrayName;
        return QString(
            "{\n"
            "    int _viz_cross_val = %1[%2];\n"
            "    viz::setValue(%3, %4, _viz_cross_val, \"%5\");\n"
            "    %5.data()[%4] = _viz_cross_val;\n"
            "}\n"
        ).arg(op.otherArrayName).arg(op.otherIndex)
         .arg(dataExpr).arg(op.index1).arg(op.arrayName);
    }
    return "";
}

QString CodeInserter::getDataExpr(const TrackedVariable& var) {
    if (var.isVector) {
        return var.name + ".data()";
    }
    return var.name;
}

QString CodeInserter::generateReverseCode(const TrackedOperation& op) {
    const TrackedVariable* var = findVar(op.arrayName);
    QString dataExpr = var ? getDataExpr(*var) : op.arrayName;
    
    // 将 reverse(arr.begin(), arr.end()) 替换为带 viz::swap 的手动循环
    // index1 = begin 偏移（如 "0"）
    // index2 = end   偏移（如 "arr.size()"）
    // end 偏移需要减 1 才能得到最后一个有效索引
    
    QString beginExpr = op.index1;   // 如 "0"
    QString endExpr = op.index2;     // 如 "arr.size()"
    
    // 处理 end 偏移减 1
    QString lastIdxExpr;
    if (endExpr == op.arrayName + ".size()") {
        lastIdxExpr = "(int)" + op.arrayName + ".size() - 1";
    } else {
        lastIdxExpr = "(int)(" + endExpr + ") - 1";
    }
    
    QString code = QString(
        "{\n"
        "    int _viz_l = %1;\n"
        "    int _viz_r = %2;\n"
        "    while (_viz_l < _viz_r) {\n"
        "        viz::swap(%3, _viz_l, _viz_r, \"%4\");\n"
        "        ++_viz_l;\n"
        "        --_viz_r;\n"
        "    }\n"
        "}\n"
    ).arg(beginExpr).arg(lastIdxExpr).arg(dataExpr).arg(op.arrayName);
    
    return code;
}

QString CodeInserter::generateFillCode(const TrackedOperation& op) {
    const TrackedVariable* var = findVar(op.arrayName);
    QString dataExpr = var ? getDataExpr(*var) : op.arrayName;

    // index1 = begin 偏移（如 "0"），index2 = end 偏移（如 "arr.size()"）
    // opcode 存放填充值表达式（如 "0"、"-1"、"n"）
    QString beginExpr = op.index1.isEmpty() ? "0" : op.index1;
    QString endExpr = op.index2;
    QString valExpr  = op.opcode.isEmpty() ? "0" : op.opcode;

    // 将 end 偏移转换为最后一个有效索引（exclusive → inclusive）
    QString endIdxExpr;
    if (endExpr == op.arrayName + ".size()") {
        endIdxExpr = "(int)" + op.arrayName + ".size()";
    } else if (endExpr.isEmpty()) {
        endIdxExpr = "(int)" + op.arrayName + ".size()";
    } else {
        endIdxExpr = "(int)(" + endExpr + ")";
    }

    // 生成逐步 viz::setValue 循环，使动画能逐格显示填充过程
    QString code = QString(
        "{\n"
        "    int _viz_fill_val = (int)(%1);\n"
        "    int _viz_fill_begin = %2;\n"
        "    int _viz_fill_end   = %3;\n"
        "    for (int _viz_i = _viz_fill_begin; _viz_i < _viz_fill_end; ++_viz_i) {\n"
        "        viz::setValue(%4, _viz_i, _viz_fill_val, \"%5\");\n"
        "        %5.data()[_viz_i] = _viz_fill_val;\n"
        "    }\n"
        "}\n"
    ).arg(valExpr).arg(beginExpr).arg(endIdxExpr).arg(dataExpr).arg(op.arrayName);

    return code;
}

QString CodeInserter::generateVecResizeCode(const TrackedOperation& op) {
    // op.index1 = 新大小表达式（如 "0", "n", "5"）
    // op.index2 = 填充值表达式（可空，resize(n) 填 0；resize(n,val) 填 val）
    QString newSizeExpr = op.index1.isEmpty() ? "0" : op.index1;
    QString fillValExpr = op.index2.isEmpty() ? "0" : op.index2;
    QString varName = op.arrayName;

    // 构造 viz::vecResize 的 label 参数字符串，用于界面显示
    // 例如 "resize(0)"、"resize(3)"、"resize(5, -1)"
    QString labelStr;
    if (op.index2.isEmpty()) {
        labelStr = QString("\"resize(%1)\"").arg(newSizeExpr);
    } else {
        labelStr = QString("\"resize(%1, %2)\"").arg(newSizeExpr).arg(fillValExpr);
    }

    // 生成插桩代码：
    //   1. 计算新大小到临时变量，避免表达式副作用
    //   2. 调用 viz::vecResize 发送 VecResize 事件
    //   3. 实际执行 vector::resize
    QString code;
    if (op.index2.isEmpty()) {
        // resize(n) — 填充值默认为 0
        code = QString(
            "{\n"
            "    int _viz_rsz = (int)(%1);\n"
            "    viz::vecResize(_viz_rsz, 0, %2, \"%3\");\n"
            "    %3.resize(_viz_rsz);\n"
            "}\n"
        ).arg(newSizeExpr).arg(labelStr).arg(varName);
    } else {
        // resize(n, val)
        code = QString(
            "{\n"
            "    int _viz_rsz = (int)(%1);\n"
            "    int _viz_rsz_val = (int)(%2);\n"
            "    viz::vecResize(_viz_rsz, _viz_rsz_val, %3, \"%4\");\n"
            "    %4.resize(_viz_rsz, _viz_rsz_val);\n"
            "}\n"
        ).arg(newSizeExpr).arg(fillValExpr).arg(labelStr).arg(varName);
    }
    return code;
}

QString CodeInserter::generateSortCode(const TrackedOperation& op) {
    const TrackedVariable* var = findVar(op.arrayName);
    QString varName = op.arrayName;
    
    // index1 = begin 偏移（如 "0"）
    // index2 = end   偏移（如 "varName.size()"）
    // opcode  = 比较器源码文本（如 "greater<int>()"、lambda 或空）
    QString beginExpr = op.index1.isEmpty() ? "0" : op.index1;
    QString endExpr   = op.index2.isEmpty() ? varName + ".size()" : op.index2;
    QString cmpExpr   = op.opcode;
    
    // 构造 sort() 调用和 comment 文字
    QString sortCall;
    QString commentText;
    
    if (cmpExpr.isEmpty()) {
        // sort(v.begin(), v.end())
        if (beginExpr == "0" && endExpr == varName + ".size()") {
            sortCall = QString("std::sort(%1.begin(), %1.end())").arg(varName);
            commentText = QString("\"sort(%1.begin(), %1.end())\"").arg(varName);
        } else {
            sortCall = QString("std::sort(%1.begin() + %2, %1.begin() + %3)")
                .arg(varName).arg(beginExpr).arg(endExpr);
            commentText = QString("\"sort(%1.begin()+%2, %1.begin()+%3)\"")
                .arg(varName).arg(beginExpr).arg(endExpr);
        }
    } else {
        // sort(v.begin(), v.end(), cmp)
        if (beginExpr == "0" && endExpr == varName + ".size()") {
            sortCall = QString("std::sort(%1.begin(), %1.end(), %2)")
                .arg(varName).arg(cmpExpr);
            commentText = QString("\"sort(%1.begin(), %1.end(), %2)\"")
                .arg(varName).arg(cmpExpr);
        } else {
            sortCall = QString("std::sort(%1.begin() + %2, %1.begin() + %3, %4)")
                .arg(varName).arg(beginExpr).arg(endExpr).arg(cmpExpr);
            commentText = QString("\"sort(%1.begin()+%2, %1.begin()+%3, %4)\"")
                .arg(varName).arg(beginExpr).arg(endExpr).arg(cmpExpr);
        }
    }
    
    // 执行真正的 std::sort，排序后逐元素 emit viz::setValue 展示最终结果
    QString code = QString(
        "{\n"
        "    %1;\n"
        "    viz::comment(%2);\n"
        "    for (int _viz_i = 0; _viz_i < (int)%3.size(); ++_viz_i) {\n"
        "        viz::setValue(_viz_i, %3[_viz_i], \"%3\");\n"
        "    }\n"
        "}\n"
    ).arg(sortCall).arg(commentText).arg(varName);
    
    return code;
}

QString CodeInserter::applyInsertions(const QString& sourceCode) {
    // 按offset降序排序（从后往前插入，避免偏移问题）
    std::sort(m_insertions.begin(), m_insertions.end(), 
        [](const Insertion& a, const Insertion& b) {
            if (a.offset != b.offset) return a.offset > b.offset;
            return a.order > b.order;
        });
    
    QString result = sourceCode;
    for (const auto& ins : m_insertions) {
        if (ins.replaceLen > 0) {
            // 替换模式：先删除原始代码，再插入新代码
            result.remove(ins.offset, ins.replaceLen);
        }
        result.insert(ins.offset, ins.text);
    }
    
    return result;
}

QString CodeInserter::generateWrapperMain(const QList<TrackedVariable>& variables) {
    QString code;
    code += "extern \"C\" __declspec(dllexport) int run_algorithm(std::vector<VizEvent>* buffer) {\n";
    code += "    viz::setEventBuffer(buffer);\n";
    
    // 生成测试数据
    for (const auto& var : variables) {
        if (!var.initValues.isEmpty()) {
            if (var.isVector) {
                code += QString("    %1 %2 = %3;\n")
                    .arg(var.type).arg(var.name).arg(var.initValues);
            } else if (var.isArray) {
                code += QString("    %1 %2 = %3;\n")
                    .arg(var.type).arg(var.name).arg(var.initValues);
            }
        } else {
            // 默认测试数据
            if (var.isVector) {
                code += QString("    std::vector<%1> %2 = {64, 34, 25, 12, 22, 11, 90};\n")
                    .arg(var.elementType).arg(var.name);
            } else if (var.isArray) {
                code += QString("    %1 %2[] = {64, 34, 25, 12, 22, 11, 90};\n")
                    .arg(var.elementType).arg(var.name);
            }
        }
    }
    
    code += "    return 0;\n";
    code += "}\n";
    
    return code;
}

QString CodeInserter::generateContainerCode(const TrackedOperation& op) {
    const TrackedVariable* var = findVar(op.arrayName);
    QString dataExpr = var ? getDataExpr(*var) : op.arrayName;
    QString varName = op.arrayName;

    switch (op.type) {
    case TrackedOperation::StlPushBack: {
        // push_back(x) → 记录 Resize 事件（带值） + 实际 push_back
        QString valueExpr = op.index1;  // 如 "x" 或 "42"
        if (valueExpr.isEmpty()) valueExpr = "0";
        return QString(
            "{\n"
            "    int _old_sz = (int)%1.size();\n"
            "    int _new_val = %2;\n"
            "    %1.push_back(_new_val);\n"
            "    viz::resize((int)%1.size(), _new_val, \"%1\");\n"
            "}\n"
        ).arg(varName).arg(valueExpr);
    }
    case TrackedOperation::StlPopBack: {
        // pop_back() → 记录 Resize 事件 + 实际 pop_back
        return QString(
            "{\n"
            "    int _old_sz = (int)%1.size();\n"
            "    %1.pop_back();\n"
            "    viz::resize((int)%1.size(), \"%1\");\n"
            "}\n"
        ).arg(varName);
    }
    case TrackedOperation::StlInsert: {
        // 动画流程：保存快照 → 真实 insert → 扩容(resize) → 逐格后移 → 写入插入值
        // insert(pos, val): index1=pos, index2=val
        // insert(pos, cnt, val): index1=pos, index2=cnt, opcode=val
        QString posExpr = op.index1.isEmpty() ? "0" : op.index1;
        if (!op.opcode.isEmpty()) {
            // insert(pos, count, val): index2=count, opcode=val
            QString cntExpr = op.index2.isEmpty() ? "1" : op.index2;
            QString valExpr = op.opcode;
            return QString(
                "{\n"
                "    int _viz_pos = (int)(%1);\n"
                "    int _viz_cnt = (int)(%2);\n"
                "    int _viz_val = (int)(%3);\n"
                "    int _viz_oldSz = (int)%4.size();\n"
                "    // 保存插入前的数据快照，用于逐格后移动画\n"
                "    std::vector<int> _viz_snap(%4);\n"
                "    // 执行真实 insert\n"
                "    %4.insert(%4.begin() + _viz_pos, _viz_cnt, _viz_val);\n"
                "    // 扩容动画（不传尾值，让新增格初始为空/0）\n"
                "    viz::resize((int)%4.size(), \"%4\");\n"
                "    // 逐格后移：从旧末尾向前，将旧值移到新位置\n"
                "    for (int _viz_i = _viz_oldSz - 1; _viz_i >= _viz_pos; --_viz_i)\n"
                "        viz::setValue(_viz_i + _viz_cnt, _viz_snap[_viz_i], \"%4\");\n"
                "    // 在插入位置写入新值\n"
                "    for (int _viz_j = 0; _viz_j < _viz_cnt; ++_viz_j)\n"
                "        viz::setValue(_viz_pos + _viz_j, _viz_val, \"%4\");\n"
                "}\n"
            ).arg(posExpr).arg(cntExpr).arg(valExpr).arg(varName);
        } else {
            // insert(pos, val): index2=val
            QString valExpr = op.index2.isEmpty() ? "0" : op.index2;
            return QString(
                "{\n"
                "    int _viz_pos = (int)(%1);\n"
                "    int _viz_val = (int)(%2);\n"
                "    int _viz_oldSz = (int)%3.size();\n"
                "    // 保存插入前的数据快照，用于逐格后移动画\n"
                "    std::vector<int> _viz_snap(%3);\n"
                "    // 执行真实 insert\n"
                "    %3.insert(%3.begin() + _viz_pos, _viz_val);\n"
                "    // 扩容动画（不传尾值，让新增格初始为空/0）\n"
                "    viz::resize((int)%3.size(), \"%3\");\n"
                "    // 逐格后移：从旧末尾向前，将旧值移到新位置\n"
                "    for (int _viz_i = _viz_oldSz - 1; _viz_i >= _viz_pos; --_viz_i)\n"
                "        viz::setValue(_viz_i + 1, _viz_snap[_viz_i], \"%3\");\n"
                "    // 在插入位置写入新值\n"
                "    viz::setValue(_viz_pos, _viz_val, \"%3\");\n"
                "}\n"
            ).arg(posExpr).arg(valExpr).arg(varName);
        }
    }
    case TrackedOperation::StlErase: {
        // erase(pos) 或 erase(first, last)
        QString posExpr = op.index1.isEmpty() ? "0" : op.index1;
        if (!op.index2.isEmpty()) {
            // erase(first, last): 区间删除后逐格左移
            return QString(
                "{\n"
                "    int _viz_first = (int)(%1);\n"
                "    int _viz_last = (int)(%2);\n"
                "    int _viz_cnt = _viz_last - _viz_first;\n"
                "    %3.erase(%3.begin() + _viz_first, %3.begin() + _viz_last);\n"
                "    viz::resize((int)%3.size(), \"%3\");\n"
                "    for (int _viz_i = _viz_first; _viz_i < (int)%3.size(); ++_viz_i)\n"
                "        viz::setValue(_viz_i, %3[_viz_i], \"%3\");\n"
                "}\n"
            ).arg(posExpr).arg(op.index2).arg(varName);
        } else {
            // erase(pos): 单元素删除后逐格左移
            return QString(
                "{\n"
                "    int _viz_pos = (int)(%1);\n"
                "    %2.erase(%2.begin() + _viz_pos);\n"
                "    viz::resize((int)%2.size(), \"%2\");\n"
                "    for (int _viz_i = _viz_pos; _viz_i < (int)%2.size(); ++_viz_i)\n"
                "        viz::setValue(_viz_i, %2[_viz_i], \"%2\");\n"
                "}\n"
            ).arg(posExpr).arg(varName);
        }
    }
    case TrackedOperation::StlClear: {
        // clear() → viz::clearContainer() + 实际 clear()
        return QString(
            "{\n"
            "    viz::clearContainer(\"%1\");\n"
            "    %1.clear();\n"
            "}\n"
        ).arg(varName);
    }
    default:
        return "";
    }
}

const TrackedVariable* CodeInserter::findVar(const QString& name) const {
    for (const auto& var : m_variables) {
        if (var.name == name) return &var;
    }
    return nullptr;
}

QString CodeInserter::generateCrossOpCode(const TrackedOperation& op) {
    const TrackedVariable* var = findVar(op.arrayName);
    QString dataExpr = var ? getDataExpr(*var) : op.arrayName;
    const TrackedVariable* otherVar = findVar(op.otherArrayName);
    QString otherDataExpr = otherVar ? getDataExpr(*otherVar) : op.otherArrayName;

    if (op.type == TrackedOperation::CrossCompare) {
        // 跨数组比较: arr[i] > brr[j]
        // 调用 viz::crossCompare 记录双方事件
        return QString(
            "viz::crossCompare(%1, %2, %3, %4, \"%5\", \"%6\")"
        ).arg(dataExpr).arg(op.index1)
         .arg(otherDataExpr).arg(op.otherIndex)
         .arg(op.arrayName).arg(op.otherArrayName);
    }
    if (op.type == TrackedOperation::CrossAssign) {
        // 跨数组赋值: arr[i] = brr[j]
        // 先读取 brr[j] 的值，再用带指针的 setValue 写入 arr[i]
        return QString(
            "{\n"
            "    int _viz_cv = %1[%2];\n"
            "    viz::setValue(%3, %4, _viz_cv, \"%5\");\n"
            "    %1[%2] = _viz_cv;\n"
            "}\n"
        ).arg(op.otherArrayName).arg(op.otherIndex)
         .arg(dataExpr).arg(op.index1).arg(op.arrayName);
    }
    return "";
}
