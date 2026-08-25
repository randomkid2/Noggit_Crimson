#include <QPushButton>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QSettings>
#include <noggit/ui/FontAwesome.hpp>

#include "expanderwidget.h"

ExpanderWidget::ExpanderWidget(QWidget *parent, bool in_designer)
    : QWidget(parent)
{
	m_in_designer = in_designer;
	m_expanded = true;

	m_collapsedIcon=Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::caretright);
	m_expandedIcon=Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::caretdown);

    m_button = new QPushButton();
    m_button->setObjectName("__qt__passive_button");
    m_button->setText("Expander");
    m_button->setFlat(true);
	//m_button->setCheckable(true);
	//m_button->setChecked(true);
    m_button->setIcon(m_expandedIcon);
    // This is the highest-traffic inline style sheet in the editor -- seven tool panels build
    // their collapsible section headers through here, so at any moment several of these are on
    // screen inside the right-hand dock. It used to read
    //     "text-align: left; font-weight: bold; border: none;"
    // and a style sheet set on the widget itself outranks the application sheet for every
    // property it names. Two of those three fought the theme and lost it real work:
    //
    //   border: none      erased the 1px top hairline the theme draws between sections, which
    //                     is the only thing separating one collapsed section from the next, and
    //                     erased the 2px transparent left border the theme reserves so a future
    //                     accent bar cannot shift the caption sideways when it appears.
    //   font-weight: bold is 700 against the theme's 600, so the header could not be tuned.
    //
    // text-align has no widget-level equivalent on QPushButton -- there is no setAlignment and
    // no style option a caller can reach -- and a theme that says nothing about this button
    // would centre the caption, which is wrong for a section header in any theme. So that one
    // property stays here as the floor, and everything else is handed back to the sheet.
    m_button->setStyleSheet("text-align: left;");
    connect(m_button, SIGNAL(clicked()), this, SLOT(buttonPressed()));

    m_stackWidget = new QStackedWidget();
    m_stackWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    m_layout = new QVBoxLayout();
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    m_layout->addWidget(m_button, 0, Qt::AlignTop);
    m_layout->addWidget(m_stackWidget);
    setLayout(m_layout);
}

void ExpanderWidget::buttonPressed()
{
    if(m_expanded)
    {
        m_expanded = false;
		    m_button->setIcon(m_collapsedIcon);
        m_stackWidget->hide();
	  }
    else
    {
        m_expanded = true;
		    m_button->setIcon(m_expandedIcon);
        m_stackWidget->show();
	}

	if(!m_in_designer && !m_settingsKey.isEmpty())
	{
		QSettings settings;
		settings.setValue(m_settingsKey,m_expanded);
	}
	
	QSize size = m_layout->sizeHint();
	int width = size.width();
	int height = size.height();

	resize(width, height);

	updateGeometry();

    emit expanderChanged(m_expanded);
}

QSize ExpanderWidget::sizeHint() const
{
	return m_layout->sizeHint();
}

void ExpanderWidget::addPage(QWidget *page)
{
    insertPage(count(), page);
}

void ExpanderWidget::removePage(int index)
{

}

int ExpanderWidget::count() const
{
    return m_stackWidget->count();
}

int ExpanderWidget::currentIndex() const
{
    return m_stackWidget->currentIndex();
}

void ExpanderWidget::insertPage(int index, QWidget *page)
{
    page->setParent(m_stackWidget);
    m_stackWidget->insertWidget(index, page);
}

void ExpanderWidget::setCurrentIndex(int index)
{
    if (index != currentIndex()) {
        m_stackWidget->setCurrentIndex(index);
        emit currentIndexChanged(index);
    }
}

QWidget* ExpanderWidget::widget(int index)
{
    return m_stackWidget->widget(index);
}

void ExpanderWidget::setExpanderTitle(QString const &newTitle)
{
    m_button->setText(newTitle);
}

QString ExpanderWidget::expanderTitle() const
{
    return m_button->text();
}

void ExpanderWidget::setExpanded(bool flag)
{
    if(flag != m_expanded)
		buttonPressed();	
}

bool ExpanderWidget::isExpanded() const
{
    return m_expanded;
}

QIcon ExpanderWidget::collapsedIcon() const
{
	return m_collapsedIcon;
}

QIcon ExpanderWidget::expandedIcon() const
{
	return m_expandedIcon;
}
	
void ExpanderWidget::setCollapsedIcon(const QIcon & icon)
{
	m_collapsedIcon=icon;
	if(!m_expanded)
	{
		m_button->setIcon(m_collapsedIcon);
	}
}

void ExpanderWidget::setExpandedIcon(const QIcon & icon)
{
	m_expandedIcon=icon;
	if(m_expanded)
	{
		m_button->setIcon(m_expandedIcon);
	}
}

QString ExpanderWidget::settingsKey() const
{
	return m_settingsKey;
}

void ExpanderWidget::setSettingsKey(QString key)
{
	m_settingsKey=key;
	
	if(!m_in_designer && !m_settingsKey.isEmpty())
	{
		QSettings settings;
		bool flag = settings.value(m_settingsKey,m_expanded).toBool();
		if(flag != m_expanded) 
			buttonPressed();
	}
}
