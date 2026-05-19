#include "ArrayVisualizer.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

ArrayVisualizer::ArrayVisualizer(QWidget *parent)
    : QWidget(parent)
    , m_blockSize(50)
    , m_blockSpacing(8)
    , m_arrowHeight(5)       // 顶部箭头区域（不再使用，保留兼容）
    , m_indexAreaHeight(45)  // 底部区域高度：25(索引) + 20(箭头)
{
    setMinimumHeight(200);
}

// ========== 数据操作 ==========

void ArrayVisualizer::setData(const std::vector<int> &data)
{
    m_values = data;
    m_blockStates.assign(data.size(), BlockState::Normal);
    recalcLayout();
    update();
}

std::vector<int> ArrayVisualizer::getData() const
{
    return m_values;
}

// ========== 色块状态 ==========

void ArrayVisualizer::setBlockState(int index, BlockState state)
{
    if (index >= 0 && index < static_cast<int>(m_blockStates.size())) {
        m_blockStates[index] = state;
        update();
    }
}

void ArrayVisualizer::resetAllBlockStates()
{
    for (auto &s : m_blockStates) {
        s = BlockState::Normal;
    }
    update();
}

// ========== 箭头操作 ==========

void ArrayVisualizer::setArrows(const std::vector<ArrowMarker> &arrows)
{
    m_arrows = arrows;
    update();
}

void ArrayVisualizer::clearArrows()
{
    m_arrows.clear();
    update();
}

// ========== 虚线框操作 ==========

void ArrayVisualizer::setDashedBoxes(const std::vector<DashedBox> &boxes)
{
    m_dashedBoxes = boxes;
    update();
}

void ArrayVisualizer::setDashedBoxes(std::initializer_list<DashedBox> boxes)
{
    m_dashedBoxes = std::vector<DashedBox>(boxes);
    update();
}

void ArrayVisualizer::clearDashedBoxes()
{
    m_dashedBoxes.clear();
    update();
}

// ========== 交换 ==========

void ArrayVisualizer::swapValues(int i, int j)
{
    if (i >= 0 && i < static_cast<int>(m_values.size()) &&
        j >= 0 && j < static_cast<int>(m_values.size())) {
        std::swap(m_values[i], m_values[j]);
        std::swap(m_blockStates[i], m_blockStates[j]);
        update();
    }
}

// ========== 布局计算 ==========

void ArrayVisualizer::recalcLayout()
{
    if (m_values.empty()) return;

    int availableWidth = width() - 40; // 左右各留20边距
    int totalSpacing = (static_cast<int>(m_values.size()) - 1) * m_blockSpacing;
    m_blockSize = (availableWidth - totalSpacing) / static_cast<int>(m_values.size());
    m_blockSize = std::clamp(m_blockSize, 30, 60);
}

QRectF ArrayVisualizer::blockRect(int index) const
{
    int totalWidth = static_cast<int>(m_values.size()) * m_blockSize
                   + (static_cast<int>(m_values.size()) - 1) * m_blockSpacing;
    int startX = (width() - totalWidth) / 2;
    int x = startX + index * (m_blockSize + m_blockSpacing);
    int y = m_arrowHeight; // 色块从顶部箭头区域下方开始
    return QRectF(x, y, m_blockSize, m_blockSize);
}

QPointF ArrayVisualizer::blockCenterBottom(int index) const
{
    QRectF rect = blockRect(index);
    return QPointF(rect.center().x(), rect.top());
}

// ========== 绘制 ==========

void ArrayVisualizer::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (m_values.empty()) {
        painter.drawText(rect(), Qt::AlignCenter, "暂无数据");
        return;
    }

    // --- 1. 绘制虚线框（在色块下层） ---
    for (const auto &box : m_dashedBoxes) {
        QRectF firstRect = blockRect(box.startIndex);
        QRectF lastRect  = blockRect(box.endIndex);
        QRectF boxRect(
            firstRect.left() - 6,
            firstRect.top() - 6,
            lastRect.right() - firstRect.left() + 12,
            lastRect.bottom() - firstRect.top() + 12
        );

        // 使用 box.color（如果未设置则回退到红色）
        QColor boxColor = box.color.isValid() ? box.color : COLOR_DASHEDBOX;
        QPen dashedPen(boxColor, 2, Qt::DashLine);
        painter.setPen(dashedPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(boxRect);

        // 标签
        if (!box.label.isEmpty()) {
            painter.setPen(boxColor);
            QFont font = painter.font();
            font.setPointSize(9);
            font.setBold(true);
            painter.setFont(font);
            painter.drawText(
                static_cast<int>(boxRect.left()),
                static_cast<int>(boxRect.top()) - 4,
                box.label
            );
        }
    }

    // --- 2. 绘制色块 ---
    for (int i = 0; i < static_cast<int>(m_values.size()); ++i) {
        QRectF rect = blockRect(i);
            QColor color;
            switch (m_blockStates[i]) {
                case BlockState::Normal:    color = COLOR_NORMAL_BLOCK; break;
                case BlockState::Active:    color = COLOR_ACTIVE_BLOCK; break;
                case BlockState::Comparing: color = COLOR_COMPARING_BLOCK; break;
                case BlockState::Swapping:  color = COLOR_SWAPPING_BLOCK; break;
                case BlockState::Sorted:    color = COLOR_SORTED_BLOCK; break;
            }

        // 色块填充
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(rect, 4, 4);

        // 色块边框
        painter.setPen(QPen(color.darker(120), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect, 4, 4);

        // 色块内数值
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPointSize(11);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(rect, Qt::AlignCenter, QString::number(m_values[i]));
    }

    // --- 3. 绘制数组下标 ---
    painter.setPen(COLOR_INDEX_TEXT);
    QFont indexFont = painter.font();
    indexFont.setPointSize(9);
    indexFont.setBold(false);
    painter.setFont(indexFont);

    for (int i = 0; i < static_cast<int>(m_values.size()); ++i) {
        QRectF rect = blockRect(i);
        painter.drawText(
            QRectF(rect.left(), rect.bottom() + 2, rect.width(), m_indexAreaHeight),
            Qt::AlignCenter,
            QString("[%1]").arg(i)
        );
    }

    // --- 4. 绘制蓝色箭头（在色块下方，向上指着节点） ---
    for (const auto &arrow : m_arrows) {
        QRectF block = blockRect(arrow.targetIndex);
        
        // 箭头在色块下方
        qreal centerX = block.center().x();
        qreal bottomY = block.bottom();  // 色块底部
        qreal arrowLen = 25;  // 箭头线长度
        
        // 箭头线：从下方指向色块底部
        QPointF lineBase(centerX, bottomY + arrowLen);
        QPointF lineTip(centerX, bottomY + 2);
        
        QPen arrowPen(COLOR_ARROW, 2);
        painter.setPen(arrowPen);
        painter.drawLine(lineBase, lineTip);
        
        // 箭头头部（向上指的三角形）
        qreal arrowSize = 6;
        QPainterPath arrowHead;
        arrowHead.moveTo(centerX, bottomY + 2);              // 三角形尖端（指向色块）
        arrowHead.lineTo(centerX - arrowSize, bottomY + arrowSize * 2 + 2);  // 左下角
        arrowHead.lineTo(centerX + arrowSize, bottomY + arrowSize * 2 + 2);  // 右下角
        arrowHead.closeSubpath();
        
        painter.setBrush(COLOR_ARROW);
        painter.setPen(Qt::NoPen);
        painter.drawPath(arrowHead);
        
        // 箭头标签（在箭头线下方）
        if (!arrow.label.isEmpty()) {
            painter.setPen(COLOR_ARROW);
            QFont labelFont = painter.font();
            labelFont.setPointSize(8);
            labelFont.setBold(true);
            painter.setFont(labelFont);
            painter.drawText(
                QRectF(centerX - 25, bottomY + arrowLen + 2, 50, 15),
                Qt::AlignCenter,
                arrow.label
            );
        }
    }
}

void ArrayVisualizer::resizeEvent(QResizeEvent *event)
{
    recalcLayout();
    QWidget::resizeEvent(event);
}

QSize ArrayVisualizer::sizeHint() const
{
    int totalWidth = static_cast<int>(m_values.size()) * (m_blockSize + m_blockSpacing);
    int totalHeight = m_arrowHeight + m_blockSize + m_indexAreaHeight;
    return QSize(totalWidth + 40, totalHeight);
}
