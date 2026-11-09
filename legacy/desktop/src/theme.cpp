#include "theme.h"
#include <QGuiApplication>
#include <QStyleHints>

static Theme* g_theme = nullptr;

Theme::Theme(QObject* parent)
    : QObject(parent), m_dark(detectScheme()) {
    g_theme = this;
    if (QGuiApplication::instance()) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, [this](Qt::ColorScheme) { setDark(detectScheme()); });
    }
}

Theme* Theme::instance() { return g_theme; }

void Theme::setDark(bool dark) {
    if (m_dark == dark) return;
    m_dark = dark;
    emit changed();
}

bool Theme::detectScheme() {
    const QString override = qEnvironmentVariable("UPIX_THEME").toLower();
    if (override == "light") return false;
    if (override == "dark") return true;
    if (QGuiApplication::instance())
        return QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
    return true;
}
