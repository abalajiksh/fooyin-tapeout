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
#include "core/tapeoutcontroller.h"
#include "tapeoutconstants.h"
#include "tapeoutsettings.h"
#include "tapeoutversion.h"

#include <utils/settings/settingsmanager.h>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSysInfo>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {
/*!
 * What pairing asks for.
 *
 * All four, because a token is minted once and a scope cannot be added to it
 * afterwards — coming back to re-pair because lyrics needed `write` is a worse
 * experience than approving one screen that says what it wants. Tapedeck shows
 * this list to the user before they approve it.
 */
constexpr auto PairingScopes = "submit read write";
} // namespace

namespace Fooyin::Tapeout {
class TapeoutPageWidget : public SettingsPageWidget
{
    Q_OBJECT

public:
    TapeoutPageWidget(TapeoutController* controller, SettingsManager* settings);

    void load() override;
    void apply() override;
    void reset() override;

private:
    void updateWidgetState();
    void testConnection();
    void showResult(const TokenInfo& info);
    void startPairing();
    void showPairingCode(const QString& userCode, int expiresInSecs);
    void finishPairing(const QString& token, const QString& userName, const QString& error);
    void showChains(const QList<ChainInfo>& chains);
    void showBindings(const QList<BindingInfo>& bindings);
    void showDryRun(const DryRunInfo& info);
    //! The chain the picker is on, as a name. Empty means "let Tapedeck resolve it".
    [[nodiscard]] QString selectedChain() const;

    TapeoutController* m_controller;
    TapedeckClient* m_client;
    SettingsManager* m_settings;

    QCheckBox* m_enabled;
    QLineEdit* m_serverUrl;
    QLineEdit* m_token;
    QPushButton* m_testButton;
    QPushButton* m_pairButton;
    QLabel* m_testResult;

    QCheckBox* m_sendQuality;
    QCheckBox* m_sendDevice;
    QCheckBox* m_sendSkips;
    QCheckBox* m_sendLyrics;
    QCheckBox* m_sendArtwork;
    QCheckBox* m_sendLoves;
    QSpinBox* m_loveThreshold;
    QLabel* m_loveThresholdLabel;
    QLabel* m_chainLabel;
    QComboBox* m_chainName;

    QPushButton* m_previewButton;
    QLabel* m_previewResult;
    QLabel* m_bindingResult;

    //! Populated from `GET /api/v1/chains`, so the binding readout can name a chain by its id.
    QList<ChainInfo> m_chains;
    bool m_pairing{false};
};

TapeoutPageWidget::TapeoutPageWidget(TapeoutController* controller, SettingsManager* settings)
    : m_controller{controller}
    , m_client{controller->client()}
    , m_settings{settings}
    , m_enabled{new QCheckBox(tr("Send listens to Tapedeck"), this)}
    , m_serverUrl{new QLineEdit(this)}
    , m_token{new QLineEdit(this)}
    , m_testButton{new QPushButton(tr("Test"), this)}
    , m_pairButton{new QPushButton(tr("Pair…"), this)}
    , m_testResult{new QLabel(this)}
    , m_sendQuality{new QCheckBox(tr("Audio quality"), this)}
    , m_sendDevice{new QCheckBox(tr("Output device"), this)}
    , m_sendSkips{new QCheckBox(tr("Skipped tracks"), this)}
    , m_sendLyrics{new QCheckBox(tr("Lyrics from your files"), this)}
    , m_sendArtwork{new QCheckBox(tr("Cover art for records Tapedeck has none for"), this)}
    , m_sendLoves{new QCheckBox(tr("Love a track when you rate it"), this)}
    , m_loveThreshold{new QSpinBox(this)}
    , m_loveThresholdLabel{new QLabel(tr("Stars to love at") + ":"_L1, this)}
    , m_chainLabel{new QLabel(tr("Signal chain") + ":"_L1, this)}
    , m_chainName{new QComboBox(this)}
    , m_previewButton{new QPushButton(tr("Preview current track"), this)}
    , m_previewResult{new QLabel(this)}
    , m_bindingResult{new QLabel(this)}
{
    m_serverUrl->setPlaceholderText(u"https://tapedeck.example.com"_s);

    // Not a secret from the person typing it, but it should not sit in plain
    // view on a shared screen either.
    m_token->setEchoMode(QLineEdit::Password);
    m_token->setPlaceholderText(tr("API token, or press Pair"));

    m_testResult->setWordWrap(true);
    m_testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_previewResult->setWordWrap(true);
    m_previewResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_bindingResult->setWordWrap(true);

    // Editable, because the picker is an improvement on free text and not a
    // replacement for it: a token with only `submit` cannot list chains, and a
    // chain typed before this existed must keep working.
    m_chainName->setEditable(true);
    m_chainName->setInsertPolicy(QComboBox::NoInsert);

    m_loveThreshold->setRange(1, 5);
    m_loveThreshold->setSuffix(tr(" ★"));

    m_pairButton->setToolTip(
        tr("Ask Tapedeck for a token instead of pasting one. fooyin shows a short code,\n"
           "you approve it in Tapedeck's settings, and the token arrives by itself —\n"
           "already carrying every scope this plugin can use."));
    m_sendQuality->setToolTip(
        tr("Report the codec, sample rate, bit depth and channel count fooyin is actually decoding.\n"
           "This is the part no other scrobbler records."));
    m_sendDevice->setToolTip(
        tr("Report which audio output is playing. Tapedeck learns each one and lets you bind it to a\n"
           "signal chain, so the chain follows your hardware without restating it."));
    m_sendSkips->setToolTip(
        tr("Submit skipped tracks as skips. Tapedeck stores them and excludes them from every count."));
    m_sendLyrics->setToolTip(
        tr("Send embedded lyrics and .lrc sidecars as the track starts, so Tapedeck has the words\n"
           "without waiting on its LRCLIB backfill. Words you corrected in Tapedeck are never\n"
           "overwritten. Needs the 'write' scope."));
    m_sendArtwork->setToolTip(
        tr("Ask Tapedeck whether it has a cover for the record, and upload the embedded one if not.\n"
           "Once per record per session, and nothing is read from disk unless it is wanted.\n"
           "Needs the 'write' scope."));
    m_sendLoves->setToolTip(
        tr("Rate a track at or above the threshold and Tapedeck loves it; drop it below and the love\n"
           "is withdrawn. Only ratings you change while fooyin is running — existing ones are left\n"
           "alone, since a love can also come from Tapedeck itself or from Last.fm.\n"
           "Needs the 'write' scope."));
    m_chainName->setToolTip(
        tr("Overrides the chain Tapedeck would resolve from the output device.\n"
           "Leave on 'Resolved by Tapedeck' unless you mean it — an unknown name resolves\n"
           "to no chain at all rather than falling through."));
    m_previewButton->setToolTip(
        tr("Submit what is playing with ?dry_run: Tapedeck resolves everything, stores nothing,\n"
           "and reports what it would have done. The only way to see your quality score and\n"
           "which rung of the chain ladder actually won."));

    auto* serverGroup  = new QGroupBox(tr("Server"), this);
    auto* serverLayout = new QGridLayout(serverGroup);

    int row{0};
    serverLayout->addWidget(m_enabled, row++, 0, 1, 4);
    serverLayout->addWidget(new QLabel(tr("Address") + ":"_L1, this), row, 0);
    serverLayout->addWidget(m_serverUrl, row++, 1, 1, 3);
    serverLayout->addWidget(new QLabel(tr("Token") + ":"_L1, this), row, 0);
    serverLayout->addWidget(m_token, row, 1);
    serverLayout->addWidget(m_testButton, row, 2);
    serverLayout->addWidget(m_pairButton, row++, 3);
    serverLayout->addWidget(m_testResult, row++, 1, 1, 3);
    serverLayout->setColumnStretch(1, 1);

    auto* reportGroup  = new QGroupBox(tr("Report"), this);
    auto* reportLayout = new QGridLayout(reportGroup);

    row = 0;
    reportLayout->addWidget(m_sendQuality, row++, 0, 1, 2);
    reportLayout->addWidget(m_sendDevice, row++, 0, 1, 2);
    reportLayout->addWidget(m_sendSkips, row++, 0, 1, 2);
    reportLayout->addWidget(m_sendLyrics, row++, 0, 1, 2);
    reportLayout->addWidget(m_sendArtwork, row++, 0, 1, 2);
    reportLayout->addWidget(m_sendLoves, row++, 0, 1, 2);
    reportLayout->addWidget(m_loveThresholdLabel, row, 0);
    reportLayout->addWidget(m_loveThreshold, row++, 1);
    reportLayout->addWidget(m_chainLabel, row, 0);
    reportLayout->addWidget(m_chainName, row++, 1);
    reportLayout->setColumnStretch(1, 1);

    auto* checkGroup  = new QGroupBox(tr("Check"), this);
    auto* checkLayout = new QGridLayout(checkGroup);

    row = 0;
    checkLayout->addWidget(m_previewButton, row, 0);
    checkLayout->addWidget(m_previewResult, row++, 1);
    checkLayout->addWidget(m_bindingResult, row++, 0, 1, 2);
    checkLayout->setColumnStretch(1, 1);

    auto* layout = new QGridLayout(this);

    row = 0;
    layout->addWidget(serverGroup, row++, 0);
    layout->addWidget(reportGroup, row++, 0);
    layout->addWidget(checkGroup, row++, 0);
    layout->setRowStretch(layout->rowCount(), 1);

    QObject::connect(m_enabled, &QCheckBox::toggled, this, &TapeoutPageWidget::updateWidgetState);
    QObject::connect(m_sendLoves, &QCheckBox::toggled, this, &TapeoutPageWidget::updateWidgetState);
    QObject::connect(m_testButton, &QPushButton::clicked, this, &TapeoutPageWidget::testConnection);
    QObject::connect(m_pairButton, &QPushButton::clicked, this, &TapeoutPageWidget::startPairing);
    QObject::connect(m_previewButton, &QPushButton::clicked, this, [this]() {
        // Against what is on screen, like Test — otherwise the preview reports on
        // the previously saved server.
        m_client->setServerUrl(m_serverUrl->text());
        m_client->setToken(m_token->text());
        m_previewResult->setText(tr("Asking…"));
        m_controller->previewCurrentTrack();
    });

    QObject::connect(m_client, &TapedeckClient::tokenValidated, this, &TapeoutPageWidget::showResult);
    QObject::connect(m_client, &TapedeckClient::pairingCode, this, &TapeoutPageWidget::showPairingCode);
    QObject::connect(m_client, &TapedeckClient::pairingFinished, this, &TapeoutPageWidget::finishPairing);
    QObject::connect(m_client, &TapedeckClient::chainsFetched, this, &TapeoutPageWidget::showChains);
    QObject::connect(m_client, &TapedeckClient::bindingsFetched, this, &TapeoutPageWidget::showBindings);
    QObject::connect(m_client, &TapedeckClient::dryRunFinished, this, &TapeoutPageWidget::showDryRun);
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
    m_sendLyrics->setChecked(m_settings->value<SendLyrics>());
    m_sendArtwork->setChecked(m_settings->value<SendArtwork>());
    m_sendLoves->setChecked(m_settings->value<SendLoves>());
    m_loveThreshold->setValue(m_settings->value<LoveThreshold>());

    // The saved name is the only entry until the list arrives, so the field
    // shows what is configured even against a `submit`-only token.
    showChains(m_chains);

    m_testResult->clear();
    m_previewResult->clear();
    m_bindingResult->clear();
    updateWidgetState();

    // Both need `read` and answer empty without it, which the readouts handle.
    m_client->fetchChains();
    m_client->fetchBindings();
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
    m_settings->set<SendLyrics>(m_sendLyrics->isChecked());
    m_settings->set<SendArtwork>(m_sendArtwork->isChecked());
    m_settings->set<SendLoves>(m_sendLoves->isChecked());
    m_settings->set<LoveThreshold>(m_loveThreshold->value());
    m_settings->set<ChainName>(selectedChain());
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
    m_settings->reset<SendLyrics>();
    m_settings->reset<SendArtwork>();
    m_settings->reset<SendLoves>();
    m_settings->reset<LoveThreshold>();
    m_settings->reset<ChainName>();
}

QString TapeoutPageWidget::selectedChain() const
{
    // Index 0 is the "resolved by Tapedeck" row, which is the empty name. Every
    // other row carries its name as data so a translated label cannot become
    // the value that gets sent.
    if(m_chainName->currentIndex() == 0) {
        return {};
    }
    const QVariant data = m_chainName->currentData();
    return data.isValid() ? data.toString() : m_chainName->currentText().trimmed();
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

void TapeoutPageWidget::startPairing()
{
    if(m_pairing) {
        m_client->cancelPairing();
        finishPairing({}, {}, tr("Pairing cancelled"));
        return;
    }

    m_client->setServerUrl(m_serverUrl->text());

    m_pairing = true;
    m_pairButton->setText(tr("Cancel"));
    m_testResult->setText(tr("Asking Tapedeck for a code…"));

    // Named per device, because that is what Tapedeck shows on the approval
    // screen and what the connection is later revoked by.
    m_client->beginPairing(QStringLiteral("%1 %2 on %3")
                               .arg(QLatin1StringView{Constants::SubmissionClient},
                                    QLatin1StringView{TAPEOUT_VERSION}, QSysInfo::machineHostName()),
                           QLatin1StringView{PairingScopes});
}

void TapeoutPageWidget::showPairingCode(const QString& userCode, int expiresInSecs)
{
    m_testResult->setText(tr("Type %1 into Tapedeck under Settings → Connections, within %2 minutes.")
                              .arg(userCode)
                              .arg(std::max(1, expiresInSecs / 60)));
}

void TapeoutPageWidget::finishPairing(const QString& token, const QString& userName, const QString& error)
{
    m_pairing = false;
    m_pairButton->setText(tr("Pair…"));

    if(token.isEmpty()) {
        m_testResult->setText(error);
        return;
    }

    m_token->setText(token);
    // The token is in that one reply and nowhere else, so it is written straight
    // through rather than waiting for Apply — a user who closes the dialog
    // without applying would otherwise have to pair again.
    m_settings->set<Settings::Tapeout::Token>(token);
    m_client->setToken(token);

    m_testResult->setText(tr("Paired as %1. Tick Enabled to start sending.").arg(userName));
    updateWidgetState();

    m_client->fetchChains();
    m_client->fetchBindings();
}

void TapeoutPageWidget::showChains(const QList<ChainInfo>& chains)
{
    m_chains = chains;

    const QString current = selectedChain().isEmpty() ? m_settings->value<Settings::Tapeout::ChainName>()
                                                      : selectedChain();

    m_chainName->clear();
    m_chainName->addItem(tr("Resolved by Tapedeck"), QString{});

    for(const ChainInfo& chain : chains) {
        // "this token's default", not "the default chain" — the distinction the
        // dry run also draws, and the reason a chain has no such flag of its own.
        const QString label = chain.isDefault ? tr("%1 (this token's default)").arg(chain.name) : chain.name;
        m_chainName->addItem(label, chain.name);
    }

    if(current.isEmpty()) {
        m_chainName->setCurrentIndex(0);
        return;
    }

    // A saved name the list does not contain is kept rather than dropped: it may
    // be a chain on a server this token cannot read, and silently clearing it
    // would change what gets submitted.
    if(const int index = m_chainName->findData(current); index >= 0) {
        m_chainName->setCurrentIndex(index);
    }
    else {
        m_chainName->addItem(tr("%1 (not found)").arg(current), current);
        m_chainName->setCurrentIndex(m_chainName->count() - 1);
    }
}

void TapeoutPageWidget::showBindings(const QList<BindingInfo>& bindings)
{
    const QString output = m_controller->currentOutputDevice();
    if(output.isEmpty()) {
        m_bindingResult->clear();
        return;
    }

    const auto binding = std::ranges::find_if(
        bindings, [&output](const BindingInfo& candidate) { return candidate.identifier == output; });

    if(binding == bindings.cend()) {
        m_bindingResult->setText(
            tr("Output “%1” is not bound to a chain yet. Tapedeck records it on the next listen, "
               "then offers it for assignment.")
                .arg(output));
        return;
    }

    if(!binding->chainId) {
        // The row exists, which means Tapedeck has seen this output — it just has
        // nothing assigned. Worth distinguishing: the fix is one tap in Tapedeck
        // rather than "play something first".
        m_bindingResult->setText(tr("Output “%1” is known to Tapedeck but has no chain assigned.").arg(output));
        return;
    }

    const auto chain = std::ranges::find_if(
        m_chains, [id = *binding->chainId](const ChainInfo& candidate) { return candidate.id == id; });
    const QString name = chain == m_chains.cend() ? tr("chain %1").arg(*binding->chainId) : chain->name;

    m_bindingResult->setText(tr("Output “%1” resolves to %2.").arg(output, name));
}

void TapeoutPageWidget::showDryRun(const DryRunInfo& info)
{
    if(!info.ok) {
        m_previewResult->setText(info.error);
        return;
    }

    QStringList parts;

    if(info.qualityScore) {
        parts.append(tr("Quality %1/100").arg(*info.qualityScore, 0, 'f', 1));
    }

    // The rung is the load-bearing half. A chain arriving from the wrong rung
    // looks exactly like one arriving from the right rung, everywhere else.
    if(!info.chainName.isEmpty()) {
        static const QHash<QString, QString> rungs{
            {u"explicit"_s, tr("named here")},
            {u"output_binding"_s, tr("from the output binding")},
            {u"token_default"_s, tr("the token's default")},
            {u"device_default"_s, tr("the device's default")},
        };
        const QString rung = rungs.value(info.chainSource, info.chainSource);
        parts.append(tr("Chain %1 (%2)").arg(info.chainName, rung));
    }
    else {
        parts.append(tr("No chain resolved"));
    }

    if(info.statusIfStored == "pending"_L1) {
        parts.append(tr("would be forwarded on"));
    }
    else if(info.statusIfStored == "imported"_L1) {
        parts.append(tr("would be stored locally only"));
    }

    if(info.duplicate) {
        parts.append(tr("already have this one"));
    }

    m_previewResult->setText(parts.join(" · "_L1));
}

void TapeoutPageWidget::showResult(const TokenInfo& info)
{
    m_testButton->setEnabled(true);

    if(!info.valid) {
        m_testResult->setText(info.error);
        updateWidgetState();
        return;
    }

    QString text = tr("Connected as %1").arg(info.userName);
    if(!info.serverVersion.isEmpty()) {
        text += tr(" — Tapedeck %1").arg(info.serverVersion);
    }

    // Scopes do not imply one another, so `submit` alone is the common case and
    // says nothing about whether the chain picker will ever work. Saying so here
    // is the whole point of asking: the alternative is a 403 at the moment the
    // user tries to use it.
    if(!info.scopes.isEmpty()) {
        if(!info.hasScope("submit"_L1)) {
            text += "\n"_L1 + tr("This token cannot submit listens. It needs the 'submit' scope.");
        }
        else if(!info.hasScope("read"_L1)) {
            text += "\n"_L1
                  + tr("Listens will be sent. Add the 'read' scope for the chain picker, or press Pair "
                       "for a token with everything.");
        }

        // Said here rather than discovered from a 403 nobody sees: these are all
        // fire-and-forget, so a token without the scope fails silently once per
        // track for as long as the boxes stay ticked.
        if((m_sendLyrics->isChecked() || m_sendArtwork->isChecked() || m_sendLoves->isChecked())
           && !info.hasScope("write"_L1)) {
            text += "\n"_L1 + tr("Lyrics, cover art and loves need the 'write' scope, which this token lacks.");
        }
    }

    m_testResult->setText(text);
    updateWidgetState();

    m_client->fetchChains();
    m_client->fetchBindings();
}

void TapeoutPageWidget::updateWidgetState()
{
    const bool enabled = m_enabled->isChecked();

    m_serverUrl->setEnabled(enabled);
    m_token->setEnabled(enabled);
    m_testButton->setEnabled(enabled);
    m_pairButton->setEnabled(enabled);
    m_sendQuality->setEnabled(enabled);
    m_sendDevice->setEnabled(enabled);
    m_sendSkips->setEnabled(enabled);
    m_sendLyrics->setEnabled(enabled);
    m_sendArtwork->setEnabled(enabled);
    m_sendLoves->setEnabled(enabled);
    m_loveThreshold->setEnabled(enabled && m_sendLoves->isChecked());
    m_loveThresholdLabel->setEnabled(enabled && m_sendLoves->isChecked());
    m_chainLabel->setEnabled(enabled);
    m_chainName->setEnabled(enabled);
    m_previewButton->setEnabled(enabled);
}

TapeoutPage::TapeoutPage(TapeoutController* controller, SettingsManager* settings, QObject* parent)
    : SettingsPage{settings->settingsDialog(), parent}
{
    setId(Constants::SettingsPage);
    setName(tr("General"));
    setCategory({tr("Integrations"), tr("Tapedeck")});
    setWidgetCreator([controller, settings] { return new TapeoutPageWidget(controller, settings); });
}
} // namespace Fooyin::Tapeout

#include "moc_tapeoutpage.cpp"
#include "tapeoutpage.moc"
