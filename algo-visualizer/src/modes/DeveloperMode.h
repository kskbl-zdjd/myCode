#ifndef DEVELOPERMODE_H
#define DEVELOPERMODE_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>

class DeveloperMode : public QWidget
{
    Q_OBJECT

public:
    explicit DeveloperMode(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        QVBoxLayout *layout = new QVBoxLayout(this);
        QLabel *label = new QLabel("开发者模式 - 功能待开发", this);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet("font-size: 24px; color: #888;");
        layout->addWidget(label);
    }
};

#endif // DEVELOPERMODE_H
