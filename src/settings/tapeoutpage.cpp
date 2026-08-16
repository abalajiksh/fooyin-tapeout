/*
 * Tapeout — Tapedeck scrobbling for fooyin
 * Copyright 2026, Ashwin Balaji
 *
 * Licensed under the MIT License. See LICENSE in the project root.
 *
 * Note: fooyin itself is GPL-3.0, so a *distributed binary* of this plugin is a
 * combined work and effectively GPL-3.0. The source in this repository is MIT.
 */

#include "tapeoutpage.h"

#include "core/tapedeckclient.h"
#include "tapeoutconstants.h"
#include "tapeoutsettings.h"

#include <utils/settings/settingsmanager.h>

#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

using namespace Qt::StringLiterals;

namespace Fooyin::Tapeout {
class TapeoutPageWidget : public SettingsPageWidget
{
    Q_OBJECT

public:
    TapeoutPageWidget(TapedeckClient* client, SettingsManager* settings);

    void load() override;
    void apply() override;
    void reset() override;

private:
    void updateWidgetState();
    void testConnection();
    void showResult(const TokenInfo& info);

    TapedeckClient* m_client;
    SettingsManager* m_settings;

    QCheckBox* m_enabled;
    QLineEdit* m_serverUrl;
    QLineEdit* m_token;
    QPushButton* m_testButton;
    QLabel* m_testResult;

    QCheckBox* m_sendQuality;
    QCheckBox* m_sendDevice;
    QCheckBox* m_sendSkips;
    QLabel* m_chainLabel;
    QLineEdit* m_chainName;
};

TapeoutPageWidget::TapeoutPageWidget(TapedeckClient* client, SettingsManager* settings)
    : m_client{client}
    , m_settings{settings}
    , m_enabled{new QCheckBox(tr("Send listens to Tapedeck"), this)}
    , m_serverUrl{new QLineEdit(this)}
    , m_token{new QLineEdit(this)}
    , m_testButton{new QPushButton(tr("Test"), this)}
    , m_testResult{new QLabel(this)}
    , m_sendQuality{new QCheckBox(tr("Audio quality"), this)}
    , m_sendDevice{new QCheckBox(tr("Output device"), this)}
    , m_sendSkips{new QCheckBox(tr("Skipped tracks"), this)}
    , m_chainLabel{new QLabel(tr("Signal chain") + ":"_L1, this)}
    , m_chainName{new QLineEdit(this)}
{
    m_serverUrl->setPlaceholderText(u"https://tapedeck.example.com"_s);

    // Not a secret from the person typing it, but it should not sit in plain
    // view on a shared screen either.
    m_token->setEchoMode(QLineEdit::Password);
    m_token->setPlaceholderText(tr("API token with the 'submit' scope"));

    m_testResult->setWordWrap(true);
    m_testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_sendQuality->setToolTip(
        tr("Report the codec, sample rate, bit depth and channel count fooyin is actually decoding.\n"
           "This is the part no other scrobbler records."));
    m_sendDevice->setToolTip(
        tr("Report which audio output is playing. Tapedeck learns each one and lets you bind it to a\n"
           "signal chain, so the chain follows your hardware without restating it."));
    m_sendSkips->setToolTip(
        tr("Submit skipped tracks as skips. Tapedeck stores them and excludes them from every count."));
    m_chainName->setToolTip(
        tr("Overrides the chain Tapedeck would resolve from the output device.\n"
           "Must match a chain name exactly — leave empty unless you mean it."));

    auto* serverGroup  = new QGroupBox(tr("Server"), this);
    auto* serverLayout = new QGridLayout(serverGroup);

    int row{0};
    serverLayout->addWidget(m_enabled, row++, 0, 1, 3);
    serverLayout->addWidget(new QLabel(tr("Address") + ":"_L1, this), row, 0);
    serverLayout->addWidget(m_serverUrl, row++, 1, 1, 2);
    serverLayout->addWidget(new QLabel(tr("Token") + ":"_L1, this), row, 0);
    serverLayout->addWidget(m_token, row, 1);
    serverLayout->addWidget(m_testButton, row++, 2);
    serverLayout->addWidget(m_testResult, row++, 1, 1, 2);
    serverLayout->setColumnStretch(1, 1);

    auto* reportGroup  = new QGroupBox(tr("Report"), this);
    auto* reportLayout = new QGridLayout(reportGroup);

    row = 0;
    reportLayout->addWidget(m_sendQuality, row++, 0, 1, 2);
    reportLayout->addWidget(m_sendDevice, row++, 0, 1, 2);
    reportLayout->addWidget(m_sendSkips, row++, 0, 1, 2);
    reportLayout->addWidget(m_chainLabel, row, 0);
    reportLayout->addWidget(m_chainName, row++, 1);
    reportLayout->setColumnStretch(1, 1);

    auto* layout = new QGridLayout(this);

    row = 0;
    layout->addWidget(serverGroup, row++, 0);
    layout->addWidget(reportGroup, row++, 0);
    layout->setRowStretch(layout->rowCount(), 1);

    QObject::connect(m_enabled, &QCheckBox::toggled, this, &TapeoutPageWidget::updateWidgetState);
    QObject::connect(m_testButton, &QPushButton::clicked, this, &TapeoutPageWidget::testConnection);
    QObject::connect(m_client, &TapedeckClient::tokenValidated, this, &TapeoutPageWidget::showResult);
}

void TapeoutPageWidget::load()
{
    using namespace Settings::Tapeout;

    m_enabled->setChecked(m_settings->value<Enabled>());
    m_serverUrl->setText(m_settings->value<ServerUrl>());
    m_token->setText(m_settings->value<Token>());
    m_sendQuality->setChecked(m_settings->value<SendQuality>());
    m_sendDevice->setChecked(m_settings->value<SendDevice>());
    m_sendSkips->setChecked(m_settings->value<SendSkips>());
    m_chainName->setText(m_settings->value<ChainName>());

    m_testResult->clear();
    updateWidgetState();
}

void TapeoutPageWidget::apply()
{
    using namespace Settings::Tapeout;

    m_settings->set<Enabled>(m_enabled->isChecked());
    m_settings->set<ServerUrl>(m_serverUrl->text().trimmed());
    m_settings->set<Token>(m_token->text().trimmed());
    m_settings->set<SendQuality>(m_sendQuality->isChecked());
    m_settings->set<SendDevice>(m_sendDevice->isChecked());
    m_settings->set<SendSkips>(m_sendSkips->isChecked());
    m_settings->set<ChainName>(m_chainName->text().trimmed());
}

void TapeoutPageWidget::reset()
{
    using namespace Settings::Tapeout;

    m_settings->reset<Enabled>();
    m_settings->reset<ServerUrl>();
    m_settings->reset<Token>();
    m_settings->reset<SendQuality>();
    m_settings->reset<SendDevice>();
    m_settings->reset<SendSkips>();
    m_settings->reset<ChainName>();
}

void TapeoutPageWidget::testConnection()
{
    // Tested against what is on screen, not what is saved — otherwise the button
    // reports on the previous token every time.
    m_client->setServerUrl(m_serverUrl->text());
    m_client->setToken(m_token->text());

    m_testResult->setText(tr("Checking…"));
    m_testButton->setEnabled(false);
    m_client->validateToken();
}

void TapeoutPageWidget::showResult(const TokenInfo& info)
{
    m_testButton->setEnabled(true);

    if(info.valid) {
        m_testResult->setText(tr("Connected as %1").arg(info.userName));
    }
    else {
        m_testResult->setText(info.error);
    }
}

void TapeoutPageWidget::updateWidgetState()
{
    const bool enabled = m_enabled->isChecked();

    m_serverUrl->setEnabled(enabled);
    m_token->setEnabled(enabled);
    m_testButton->setEnabled(enabled);
    m_sendQuality->setEnabled(enabled);
    m_sendDevice->setEnabled(enabled);
    m_sendSkips->setEnabled(enabled);
    m_chainLabel->setEnabled(enabled);
    m_chainName->setEnabled(enabled);
}

TapeoutPage::TapeoutPage(TapedeckClient* client, SettingsManager* settings, QObject* parent)
    : SettingsPage{settings->settingsDialog(), parent}
{
    setId(Constants::SettingsPage);
    setName(tr("General"));
    setCategory({tr("Integrations"), tr("Tapedeck")});
    setWidgetCreator([client, settings] { return new TapeoutPageWidget(client, settings); });
}
} // namespace Fooyin::Tapeout

#include "moc_tapeoutpage.cpp"
#include "tapeoutpage.moc"
