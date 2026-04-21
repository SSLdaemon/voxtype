#include <QApplication>
#include <QColor>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLockFile>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QWindow>
#include <QWidget>

#include <LayerShellQt/Window>

#include <cmath>

namespace {

enum class OverlayState {
    Stopped,
    Idle,
    Recording,
    Transcribing,
};

struct OverlayConfig {
    QString screenSelector = QStringLiteral("primary");
    QString stateFile = QStringLiteral("auto");
    int width = 176;
    int height = 24;
    int rightMargin = 22;
    int bottomGap = 8;
    int cornerRadius = 12;
    int barCount = 14;
    int barSpacing = 4;
    bool showTranscribing = false;
    bool showIdle = false;
    QColor background = QColor(QStringLiteral("#0e1318"));
    QColor border = QColor(QStringLiteral("#76f2de"));
    QColor recording = QColor(QStringLiteral("#76f2de"));
    QColor recordingPeak = QColor(QStringLiteral("#f4fffd"));
    QColor transcribing = QColor(QStringLiteral("#f2b661"));
};

QString defaultStateFile()
{
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty()) {
        return QDir(runtime).filePath(QStringLiteral("voxtype/state"));
    }
    return QStringLiteral("/tmp/voxtype/state");
}

QColor readColor(QSettings &settings, const QString &key, const QColor &fallback)
{
    const QString raw = settings.value(key, fallback.name(QColor::HexRgb)).toString().trimmed();
    const QColor parsed(raw);
    return parsed.isValid() ? parsed : fallback;
}

OverlayState parseState(const QString &rawState)
{
    const QString normalized = rawState.trimmed().toLower();
    if (normalized == QStringLiteral("recording")) {
        return OverlayState::Recording;
    }
    if (normalized == QStringLiteral("transcribing")) {
        return OverlayState::Transcribing;
    }
    if (normalized == QStringLiteral("idle")) {
        return OverlayState::Idle;
    }
    return OverlayState::Stopped;
}

QString stateName(OverlayState state)
{
    switch (state) {
    case OverlayState::Recording:
        return QStringLiteral("recording");
    case OverlayState::Transcribing:
        return QStringLiteral("transcribing");
    case OverlayState::Idle:
        return QStringLiteral("idle");
    case OverlayState::Stopped:
    default:
        return QStringLiteral("stopped");
    }
}

OverlayConfig loadConfig(const QString &configPath)
{
    OverlayConfig config;
    if (!QFile::exists(configPath)) {
        config.stateFile = defaultStateFile();
        return config;
    }

    QSettings settings(configPath, QSettings::IniFormat);

    settings.beginGroup(QStringLiteral("overlay"));
    config.screenSelector = settings.value(QStringLiteral("screen"), config.screenSelector).toString().trimmed();
    config.width = settings.value(QStringLiteral("width"), config.width).toInt();
    config.height = settings.value(QStringLiteral("height"), config.height).toInt();
    config.rightMargin = settings.value(QStringLiteral("right_margin"), config.rightMargin).toInt();
    config.bottomGap = settings.value(QStringLiteral("bottom_gap"), config.bottomGap).toInt();
    config.cornerRadius = settings.value(QStringLiteral("corner_radius"), config.cornerRadius).toInt();
    config.barCount = settings.value(QStringLiteral("bar_count"), config.barCount).toInt();
    config.barSpacing = settings.value(QStringLiteral("bar_spacing"), config.barSpacing).toInt();
    config.showTranscribing = settings.value(QStringLiteral("show_transcribing"), config.showTranscribing).toBool();
    config.showIdle = settings.value(QStringLiteral("show_idle"), config.showIdle).toBool();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("voxtype"));
    config.stateFile = settings.value(QStringLiteral("state_file"), config.stateFile).toString().trimmed();
    settings.endGroup();

    settings.beginGroup(QStringLiteral("style"));
    config.background = readColor(settings, QStringLiteral("background"), config.background);
    config.border = readColor(settings, QStringLiteral("border"), config.border);
    config.recording = readColor(settings, QStringLiteral("recording"), config.recording);
    config.recordingPeak = readColor(settings, QStringLiteral("recording_peak"), config.recordingPeak);
    config.transcribing = readColor(settings, QStringLiteral("transcribing"), config.transcribing);
    settings.endGroup();

    if (config.stateFile.isEmpty() || config.stateFile == QStringLiteral("auto")) {
        config.stateFile = defaultStateFile();
    }

    config.width = std::max(config.width, 160);
    config.height = std::max(config.height, 22);
    config.cornerRadius = std::max(config.cornerRadius, config.height / 2);
    config.barCount = std::max(config.barCount, 8);
    config.barSpacing = std::max(config.barSpacing, 2);
    config.rightMargin = std::max(config.rightMargin, 0);
    config.bottomGap = std::max(config.bottomGap, 0);

    return config;
}

class OverlayWidget final : public QWidget {
public:
    explicit OverlayWidget(OverlayConfig config, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_config(std::move(config))
        , m_barLevels(m_config.barCount, 0.0)
    {
        setObjectName(QStringLiteral("voxtypeListeningOverlay"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(m_config.width, m_config.height);
        setWindowFlags(Qt::Window
            | Qt::FramelessWindowHint
            | Qt::WindowStaysOnTopHint
            | Qt::WindowDoesNotAcceptFocus
            | Qt::WindowTransparentForInput
            | Qt::NoDropShadowWindowHint);

        winId();
        ensureLayerShell();
        reposition();
        setVisible(false);

        connect(&m_stateTimer, &QTimer::timeout, this, [this] { syncState(); });
        m_stateTimer.start(120);

        connect(&m_animationTimer, &QTimer::timeout, this, [this] { advanceAnimation(); });
        m_animationTimer.start(33);

        for (QScreen *screen : QGuiApplication::screens()) {
            connect(screen, &QScreen::geometryChanged, this, [this] { reposition(); });
            connect(screen, &QScreen::availableGeometryChanged, this, [this] { reposition(); });
        }

        connect(qApp, &QGuiApplication::primaryScreenChanged, this, [this] {
            reposition();
        });
        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
            connect(screen, &QScreen::geometryChanged, this, [this] { reposition(); });
            connect(screen, &QScreen::availableGeometryChanged, this, [this] { reposition(); });
            reposition();
        });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this] {
            reposition();
        });

        syncState();
    }

    QString resolvedStateFile() const
    {
        return m_config.stateFile;
    }

    OverlayState state() const
    {
        return m_state;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_visibility <= 0.01) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF bounds = rect().adjusted(1.0, 1.0, -1.0, -1.0);
        const qreal radius = std::min<qreal>(m_config.cornerRadius, bounds.height() / 2.0);

        QColor frameColor = frameAccent();
        frameColor.setAlphaF(0.24 * m_visibility);

        QColor bg = m_config.background;
        bg.setAlphaF(0.72 * m_visibility);

        QColor glow = frameAccent();
        glow.setAlphaF(0.08 * m_visibility);

        for (int i = 0; i < 3; ++i) {
            const QRectF glowRect = bounds.adjusted(-i, -i, i, i);
            painter.setPen(Qt::NoPen);
            painter.setBrush(glow);
            painter.drawRoundedRect(glowRect, radius + i, radius + i);
        }

        painter.setPen(QPen(frameColor, 1.0));
        painter.setBrush(bg);
        painter.drawRoundedRect(bounds, radius, radius);

        const QRectF content = bounds.adjusted(14.0, 7.0, -14.0, -7.0);
        const qreal baselineY = content.bottom();
        const qreal minBarHeight = 3.0;
        const qreal usableHeight = std::max<qreal>(content.height(), minBarHeight + 1.0);
        const qreal spacing = m_config.barSpacing;
        const qreal totalSpacing = spacing * (m_config.barCount - 1);
        const qreal barWidth = std::max<qreal>(3.0, (content.width() - totalSpacing) / m_config.barCount);

        QColor guide = frameAccent();
        guide.setAlphaF(0.12 * m_visibility);
        painter.setPen(QPen(guide, 1.0));
        painter.drawLine(QPointF(content.left(), baselineY + 0.5), QPointF(content.right(), baselineY + 0.5));

        painter.setPen(Qt::NoPen);
        for (int i = 0; i < m_barLevels.size(); ++i) {
            const qreal level = std::clamp(m_barLevels.at(i), 0.0, 1.0);
            const qreal h = minBarHeight + level * (usableHeight - minBarHeight);
            const qreal x = content.left() + i * (barWidth + spacing);
            const QRectF barRect(x, baselineY - h, barWidth, h);

            QLinearGradient gradient(barRect.topLeft(), barRect.bottomLeft());
            QColor baseColor = barBaseColor();
            QColor topColor = barPeakColor();
            baseColor.setAlphaF((0.70 + level * 0.15) * m_visibility);
            topColor.setAlphaF((0.95 + level * 0.05) * m_visibility);
            gradient.setColorAt(0.0, topColor);
            gradient.setColorAt(0.7, baseColor);
            gradient.setColorAt(1.0, baseColor.darker(125));
            painter.setBrush(gradient);
            painter.drawRoundedRect(barRect, std::min<qreal>(barWidth / 2.0, 2.5), 2.5);
        }
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        ensureLayerShell();
        reposition();
        if (QWindow *handle = windowHandle()) {
            handle->setFlag(Qt::WindowDoesNotAcceptFocus, true);
            handle->setFlag(Qt::WindowTransparentForInput, true);
        }
    }

private:
    void ensureLayerShell()
    {
        if (m_layerShellConfigured) {
            return;
        }

        QWindow *handle = windowHandle();
        if (!handle) {
            return;
        }

        auto *layerWindow = LayerShellQt::Window::get(handle);
        if (!layerWindow) {
            return;
        }

        m_layerWindow = layerWindow;
        m_layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        LayerShellQt::Window::Anchors anchors;
        anchors |= LayerShellQt::Window::AnchorRight;
        anchors |= LayerShellQt::Window::AnchorBottom;
        m_layerWindow->setAnchors(anchors);
        m_layerWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        m_layerWindow->setActivateOnShow(false);
        m_layerWindow->setExclusiveZone(0);
        m_layerWindow->setDesiredSize(size());
        m_layerWindow->setScope(QStringLiteral("voxtype-listening-overlay"));
        m_layerShellConfigured = true;
    }

    void syncState()
    {
        QFile file(m_config.stateFile);
        OverlayState nextState = OverlayState::Stopped;
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            nextState = parseState(stream.readAll());
        }

        if (nextState != m_state) {
            m_state = nextState;
            if (wantsVisible()) {
                reposition();
                show();
            }
        }
    }

    void advanceAnimation()
    {
        m_time += 0.075;

        const qreal targetVisibility = wantsVisible() ? 1.0 : 0.0;
        m_visibility += (targetVisibility - m_visibility) * 0.18;

        if (m_visibility < 0.02 && !wantsVisible()) {
            if (isVisible()) {
                hide();
            }
            return;
        }

        if (!isVisible()) {
            reposition();
            show();
        }

        for (int i = 0; i < m_barLevels.size(); ++i) {
            const qreal x = m_barLevels.size() <= 1 ? 0.5 : static_cast<qreal>(i) / (m_barLevels.size() - 1);
            qreal target = 0.06;

            if (m_state == OverlayState::Recording) {
                const qreal waveA = (std::sin(m_time * 2.8 + i * 0.62) + 1.0) * 0.5;
                const qreal waveB = (std::sin(m_time * 1.7 - i * 0.37) + 1.0) * 0.5;
                const qreal centerBias = 1.0 - std::min<qreal>(1.0, std::abs(x - 0.5) * 1.7);
                target = 0.22 + 0.78 * std::clamp((waveA * 0.54) + (waveB * 0.22) + (centerBias * 0.24), 0.0, 1.0);
            } else if (m_state == OverlayState::Transcribing) {
                const qreal sweep = (std::sin(m_time * 1.5 - i * 0.48) + 1.0) * 0.5;
                target = 0.12 + 0.28 * sweep;
            } else if (m_state == OverlayState::Idle && m_config.showIdle) {
                const qreal pulse = (std::sin(m_time * 0.8 + i * 0.22) + 1.0) * 0.5;
                target = 0.08 + 0.10 * pulse;
            }

            m_barLevels[i] += (target - m_barLevels[i]) * (m_state == OverlayState::Recording ? 0.24 : 0.12);
        }

        update();
    }

    bool wantsVisible() const
    {
        if (m_state == OverlayState::Recording) {
            return true;
        }
        if (m_state == OverlayState::Transcribing) {
            return m_config.showTranscribing;
        }
        if (m_state == OverlayState::Idle) {
            return m_config.showIdle;
        }
        return false;
    }

    QColor frameAccent() const
    {
        return m_state == OverlayState::Transcribing ? m_config.transcribing : m_config.border;
    }

    QColor barBaseColor() const
    {
        return m_state == OverlayState::Transcribing ? m_config.transcribing : m_config.recording;
    }

    QColor barPeakColor() const
    {
        return m_state == OverlayState::Transcribing ? m_config.transcribing.lighter(125) : m_config.recordingPeak;
    }

    QScreen *targetScreen() const
    {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (screens.isEmpty()) {
            return QGuiApplication::primaryScreen();
        }

        if (m_config.screenSelector.compare(QStringLiteral("primary"), Qt::CaseInsensitive) == 0) {
            return QGuiApplication::primaryScreen();
        }

        bool ok = false;
        const int index = m_config.screenSelector.toInt(&ok);
        if (ok && index >= 0 && index < screens.size()) {
            return screens.at(index);
        }

        for (QScreen *screen : screens) {
            if (screen->name() == m_config.screenSelector) {
                return screen;
            }
        }

        return QGuiApplication::primaryScreen();
    }

    void reposition()
    {
        QScreen *screen = targetScreen();
        if (!screen) {
            return;
        }

        const QRect geometry = screen->geometry();
        const QRect available = screen->availableGeometry();
        const int reservedRight = std::max(0, geometry.right() - available.right());
        const int reservedBottom = std::max(0, geometry.bottom() - available.bottom());

        if (m_layerWindow) {
            if (QWindow *handle = windowHandle()) {
                handle->setScreen(screen);
            }
            m_layerWindow->setScreen(screen);
            m_layerWindow->setDesiredSize(size());
            m_layerWindow->setMargins(QMargins(
                0,
                0,
                reservedRight + m_config.rightMargin,
                reservedBottom + m_config.bottomGap));
            return;
        }

        const int x = available.right() - width() - m_config.rightMargin + 1;
        const int y = available.bottom() - height() - m_config.bottomGap + 1;
        move(x, y);
    }

    OverlayConfig m_config;
    QVector<qreal> m_barLevels;
    QTimer m_stateTimer;
    QTimer m_animationTimer;
    LayerShellQt::Window *m_layerWindow = nullptr;
    OverlayState m_state = OverlayState::Stopped;
    qreal m_time = 0.0;
    qreal m_visibility = 0.0;
    bool m_layerShellConfigured = false;
};

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Voxtype Listening Overlay"));
    QApplication::setOrganizationName(QStringLiteral("local"));
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Slim Plasma overlay for voxtype recording state."));
    parser.addHelpOption();

    const QString defaultConfigPath = QDir::homePath() + QStringLiteral("/.config/voxtype-overlay/config.ini");
    QCommandLineOption configOption(
        QStringList() << QStringLiteral("c") << QStringLiteral("config"),
        QStringLiteral("Path to overlay config file."),
        QStringLiteral("file"),
        defaultConfigPath);
    QCommandLineOption stateFileOption(
        QStringList() << QStringLiteral("state-file"),
        QStringLiteral("Override voxtype state file path."),
        QStringLiteral("file"));
    QCommandLineOption printStatusOption(
        QStringList() << QStringLiteral("print-status"),
        QStringLiteral("Print resolved state file path and current state, then exit."));

    parser.addOption(configOption);
    parser.addOption(stateFileOption);
    parser.addOption(printStatusOption);
    parser.process(app);

    OverlayConfig config = loadConfig(parser.value(configOption));
    if (parser.isSet(stateFileOption)) {
        config.stateFile = parser.value(stateFileOption).trimmed();
    }
    if (config.stateFile.isEmpty() || config.stateFile == QStringLiteral("auto")) {
        config.stateFile = defaultStateFile();
    }

    QFile stateFile(config.stateFile);
    OverlayState currentState = OverlayState::Stopped;
    if (stateFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&stateFile);
        currentState = parseState(stream.readAll());
    }

    if (parser.isSet(printStatusOption)) {
        QTextStream(stdout) << "state_file=" << config.stateFile << "\nstate=" << stateName(currentState) << "\n";
        return 0;
    }

    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString lockPath = !runtimeDir.isEmpty()
        ? QDir(runtimeDir).filePath(QStringLiteral("voxtype-listening-overlay.lock"))
        : QDir(QDir::tempPath()).filePath(QStringLiteral("voxtype-listening-overlay.lock"));
    QLockFile singleInstanceLock(lockPath);
    singleInstanceLock.setStaleLockTime(0);
    if (!singleInstanceLock.tryLock(0)) {
        return 0;
    }

    OverlayWidget widget(std::move(config));
    return app.exec();
}
