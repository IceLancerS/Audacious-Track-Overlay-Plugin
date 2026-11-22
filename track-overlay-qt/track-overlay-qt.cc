/*
 * Track Overlay - Qt general plugin for Audacious 4.x
 *
 * Shows a small on-screen overlay when a new track starts:
 *
 *   +-------------------------------------+
 *   |  Artist                       3:45  |
 *   |  Title                              |
 *   +-------------------------------------+
 *   |  FLAC  -  1411 kbps  -  44.1 kHz  -  Stereo
 *   +-------------------------------------+
 *
 * The overlay is purely visual: it is click-through, never takes focus, has
 * no buttons, fades in, stays for a few seconds and fades out.
 * Pause / unpause / seeking do not trigger it, only a new track does.
 *
 * No Q_OBJECT / signals / slots, so no moc step is needed.
 */

#include <QGuiApplication>
#include <QMetaObject>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>
#include <libaudcore/tuple.h>

#define CFG "track_overlay"

static QString qs(const String & s)
{
    const char * c = (const char *) s;
    return c ? QString::fromUtf8(c) : QString();
}

static QString fmt_time(int ms)
{
    int s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

// File extension of the playing item ("MP3", "FLAC", ...)
static QString ext_of(const String & filename)
{
    QString f = qs(filename);

    // strip sub-song / fragment suffixes such as "file.cue?2" or "#x"
    int q = f.indexOf('?');
    if (q >= 0) f.truncate(q);
    q = f.indexOf('#');
    if (q >= 0) f.truncate(q);

    if (f.startsWith("http://") || f.startsWith("https://"))
    {
        int path = f.indexOf('/', f.indexOf("//") + 2);
        if (path < 0)
            return "STREAM";
        f = f.mid(path);
    }

    int slash = f.lastIndexOf('/');
    int dot = f.lastIndexOf('.');
    if (dot > slash && dot >= 0)
    {
        QString e = f.mid(dot + 1);
        if (e.size() >= 2 && e.size() <= 5)
            return e.toUpper();
    }
    return QString();
}

// One coloured piece of the file-info line
struct Seg
{
    QString text;
    QColor color;
};

static bool is_lossless(const QString & ext)
{
    static const QStringList lossless = {"FLAC", "WAV", "WAVE", "APE", "WV", "AIF",
                                         "AIFF", "TTA", "ALAC", "DSF", "DFF"};
    return lossless.contains(ext);
}

// Quality tier -> colour (used for the format and the bitrate)
static QColor quality_color(const QString & ext, int kbps)
{
    if (is_lossless(ext)) return QColor(92, 214, 138);    // green : lossless
    if (kbps >= 256)      return QColor(90, 176, 255);    // blue  : high-bitrate lossy
    if (kbps >= 160)      return QColor(242, 193, 78);    // amber : medium
    if (kbps > 0)         return QColor(240, 112, 90);    // red   : low bitrate
    return QColor(150, 158, 176);                         // grey  : unknown
}

/* ------------------------------------------------------------------------ *
 *  The overlay window
 * ------------------------------------------------------------------------ */

class Overlay : public QWidget
{
public:
    Overlay()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                               Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);

        m_fArtist = font(); m_fArtist.setPointSizeF(10.5);
        m_fTitle  = font(); m_fTitle.setPointSizeF(14.0); m_fTitle.setBold(true);
        m_fTime   = font(); m_fTime.setPointSizeF(12.0);
        m_fInfo   = font(); m_fInfo.setPointSizeF(9.5);

        m_anim.setTargetObject(this);
        m_anim.setPropertyName("windowOpacity");

        m_hide.setSingleShot(true);
        QObject::connect(&m_hide, &QTimer::timeout, [this]() { fade(0.0, 400); });
        QObject::connect(&m_anim, &QPropertyAnimation::finished, [this]() {
            if (windowOpacity() < 0.01)
                hide();
        });
    }

    // New track started
    void announce(bool sample = false)
    {
        if (!sample && !aud_drct_get_ready())
            return;

        load(sample);
        relayout();

        m_hide.stop();
        if (!(isVisible() && windowOpacity() > 0.05))
        {
            setWindowOpacity(0.0);
            show();
        }
        fade(target_opacity(), 250);

        int secs = aud_get_int(CFG, "overlay_seconds");
        if (secs < 1 || secs > 30) secs = 5;
        m_hide.start(secs * 1000);
    }

    // Tags / bitrate arrived late: update what is already on screen
    void refresh()
    {
        if (!isVisible() || m_sample)
            return;
        load(false);
        relayout();
        update();
    }

    // Playback stopped
    void dismiss()
    {
        m_hide.stop();
        if (isVisible())
            fade(0.0, 200);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);

        const int pad = 16;
        const QColor card(20, 22, 28, 230);

        p.setPen(Qt::NoPen);
        p.setBrush(card);
        p.drawRoundedRect(m_r1, 12, 12);
        if (m_showInfo)
            p.drawRoundedRect(m_r2, 10, 10);

        // --- card 1: artist, title, time
        QFontMetrics fmA(m_fArtist), fmT(m_fTitle);
        int y = m_r1.top() + pad;

        if (!m_artist.isEmpty())
        {
            p.setFont(m_fArtist);
            p.setPen(QColor(165, 172, 188));
            p.drawText(QRect(pad, y, m_availW, fmA.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       fmA.elidedText(m_artist, Qt::ElideRight, m_availW));
            y += fmA.height() + 2;
        }

        p.setFont(m_fTitle);
        p.setPen(QColor(245, 246, 250));
        p.drawText(QRect(pad, y, m_availW, fmT.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fmT.elidedText(m_title, Qt::ElideRight, m_availW));

        if (!m_time.isEmpty())
        {
            p.setFont(m_fTime);
            p.setPen(QColor(200, 205, 218));
            p.drawText(m_r1.adjusted(pad, 0, -pad, 0), Qt::AlignRight | Qt::AlignVCenter, m_time);
        }

        // --- card 2: file info
        if (m_showInfo)
        {
            QFontMetrics fmI(m_fInfo);
            p.setFont(m_fInfo);
            int x = m_r2.left() + pad;
            for (const Seg & sg : m_segs)
            {
                const int sw = fmI.horizontalAdvance(sg.text);
                p.setPen(sg.color);
                p.drawText(QRect(x, m_r2.top(), sw + 2, m_r2.height()),
                           Qt::AlignLeft | Qt::AlignVCenter, sg.text);
                x += sw;
            }
        }
    }

private:
    double target_opacity() const
    {
        int o = aud_get_int(CFG, "overlay_opacity");
        if (o < 20 || o > 100) o = 95;
        return o / 100.0;
    }

    void fade(double to, int ms)
    {
        m_anim.stop();
        m_anim.setDuration(ms);
        m_anim.setStartValue(windowOpacity());
        m_anim.setEndValue(to);
        m_anim.start();
    }

    // Build the coloured file-info line
    void build_info(const QString & ext, int br, int sr, int ch)
    {
        const bool coded = aud_get_bool(CFG, "color_code");
        const QColor neutral(150, 158, 176), dim(95, 101, 117);
        const QString dot = QString(" ") + QChar(0x00B7) + " ";
        const int kbps = (br > 0) ? (br + 500) / 1000 : 0;
        const QColor quality = coded ? quality_color(ext, kbps) : neutral;

        m_segs.clear();
        auto add = [&](const QString & text, const QColor & c) {
            if (!m_segs.isEmpty())
                m_segs.append({dot, dim});
            m_segs.append({text, c});
        };

        if (!ext.isEmpty())
            add(ext, quality);
        if (kbps > 0)
            add(QString("%1 kbps").arg(kbps), quality);
        if (sr > 0)
            add(QString("%1 kHz").arg(sr / 1000.0, 0, 'f', (sr % 1000) ? 1 : 0),
                (coded && sr >= 88200) ? QColor(181, 140, 255) : neutral);   // violet: hi-res
        if (ch == 1)      add("Mono", neutral);
        else if (ch == 2) add("Stereo", neutral);
        else if (ch > 2)  add(QString("%1 ch").arg(ch), neutral);
    }

    // Read everything we show from Audacious
    void load(bool sample)
    {
        m_sample = sample;

        if (sample)
        {
            m_artist = "Artist Name";
            m_title = "Track Title";
            m_time = "3:45";
            build_info("FLAC", 1411000, 44100, 2);
            return;
        }

        Tuple t = aud_drct_get_tuple();
        m_artist = qs(t.get_str(Tuple::Artist));
        m_title = qs(t.get_str(Tuple::Title));
        if (m_title.isEmpty())
            m_title = qs(aud_drct_get_title());

        int len = aud_drct_get_length();
        m_time = (len > 0) ? fmt_time(len) : QString();

        int br = 0, sr = 0, ch = 0;
        aud_drct_get_info(br, sr, ch);
        build_info(ext_of(aud_drct_get_filename()), br, sr, ch);
    }

    // Measure text, size the window, put it in the chosen screen corner
    void relayout()
    {
        const int pad = 16, gap = 8, timeGap = 18;
        m_showInfo = aud_get_bool(CFG, "show_info") && !m_segs.isEmpty();

        QFontMetrics fmA(m_fArtist), fmT(m_fTitle), fmTime(m_fTime), fmI(m_fInfo);

        const int timeW = m_time.isEmpty() ? 0 : fmTime.horizontalAdvance(m_time);
        const int timeSpace = timeW ? timeGap + timeW : 0;
        const int textW = qMax(fmA.horizontalAdvance(m_artist), fmT.horizontalAdvance(m_title));
        int infoW = 0;
        if (m_showInfo)
            for (const Seg & sg : m_segs)
                infoW += fmI.horizontalAdvance(sg.text);
        const int w = qBound(300, qMax(textW + timeSpace, infoW) + 2 * pad, 640);
        m_availW = w - 2 * pad - timeSpace;

        const int artistH = m_artist.isEmpty() ? 0 : fmA.height() + 2;
        const int h1 = 2 * pad + artistH + fmT.height();
        const int h2 = m_showInfo ? 2 * 10 + fmI.height() : 0;

        m_r1 = QRect(0, 0, w, h1);
        m_r2 = m_showInfo ? QRect(0, h1 + gap, w, h2) : QRect();
        resize(w, h1 + (m_showInfo ? gap + h2 : 0));

        // position
        QScreen * sc = QGuiApplication::primaryScreen();
        if (!sc)
            return;
        const QRect g = sc->availableGeometry();   // excludes the taskbar
        const int margin = 24;
        const int corner = aud_get_int(CFG, "overlay_corner");   // 0 TL, 1 TR, 2 BL, 3 BR
        const int x = (corner == 1 || corner == 3) ? g.right() - width() - margin + 1 : g.left() + margin;
        const int y = (corner == 2 || corner == 3) ? g.bottom() - height() - margin + 1 : g.top() + margin;
        move(x, y);
    }

    QString m_artist, m_title, m_time;
    QVector<Seg> m_segs;
    QFont m_fArtist, m_fTitle, m_fTime, m_fInfo;
    QRect m_r1, m_r2;
    int m_availW = 200;
    bool m_showInfo = false;
    bool m_sample = false;

    QTimer m_hide;
    QPropertyAnimation m_anim;
};

static Overlay * overlay = nullptr;

/* ------------------------------------------------------------------------ *
 *  Audacious hooks (marshalled onto the Qt/main thread to be safe)
 * ------------------------------------------------------------------------ */

static void hook_ready(void *, void *)
{
    if (overlay)
        QMetaObject::invokeMethod(overlay, []() { overlay->announce(); }, Qt::QueuedConnection);
}

static void hook_changed(void *, void *)
{
    if (overlay)
        QMetaObject::invokeMethod(overlay, []() { overlay->refresh(); }, Qt::QueuedConnection);
}

static void hook_stop(void *, void *)
{
    if (overlay)
        QMetaObject::invokeMethod(overlay, []() { overlay->dismiss(); }, Qt::QueuedConnection);
}

// "Preview" button in the settings window
static void preview_cb()
{
    if (!overlay)
        return;
    overlay->announce(!aud_drct_get_playing());   // real track if playing, sample text otherwise
}

/* ------------------------------------------------------------------------ *
 *  Plugin glue
 * ------------------------------------------------------------------------ */

class TrackOverlayPlugin : public GeneralPlugin
{
public:
    static const char about[];
    static const ComboItem corner_items[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {N_("Track Overlay"), PACKAGE, about, &prefs, PluginQtOnly};

    constexpr TrackOverlayPlugin() : GeneralPlugin(info, false) {}

    bool init();
    void cleanup();
};

EXPORT TrackOverlayPlugin aud_plugin_instance;

const char TrackOverlayPlugin::about[] =
    N_("Track Overlay\n\n"
       "Shows the artist, title, length and file info in a small on-screen\n"
       "overlay when a new track starts. The overlay is click-through and\n"
       "never takes focus.");

const ComboItem TrackOverlayPlugin::corner_items[] = {
    ComboItem(N_("Top left"), 0),
    ComboItem(N_("Top right"), 1),
    ComboItem(N_("Bottom left"), 2),
    ComboItem(N_("Bottom right"), 3)
};

const PreferencesWidget TrackOverlayPlugin::widgets[] = {
    WidgetSpin(N_("Display time:"), WidgetInt(CFG, "overlay_seconds"), {1, 30, 1, N_("s")}),
    WidgetCombo(N_("Position:"), WidgetInt(CFG, "overlay_corner"), {{corner_items}}),
    WidgetSpin(N_("Opacity:"), WidgetInt(CFG, "overlay_opacity"), {20, 100, 5, N_("%")}),
    WidgetCheck(N_("Show file info card"), WidgetBool(CFG, "show_info")),
    WidgetCheck(N_("Color-code file info by quality"), WidgetBool(CFG, "color_code")),
    WidgetButton(N_("Preview"), {preview_cb})
};

const PluginPreferences TrackOverlayPlugin::prefs = {{widgets}};

static const char * const defaults[] = {
    "overlay_seconds", "5",
    "overlay_corner", "3",
    "overlay_opacity", "95",
    "show_info", "TRUE",
    "color_code", "TRUE",
    nullptr
};

bool TrackOverlayPlugin::init()
{
    aud_config_set_defaults(CFG, defaults);

    overlay = new Overlay;

    hook_associate("playback ready", hook_ready, nullptr);
    hook_associate("info change", hook_changed, nullptr);
    hook_associate("tuple change", hook_changed, nullptr);
    hook_associate("title change", hook_changed, nullptr);
    hook_associate("playback stop", hook_stop, nullptr);
    return true;
}

void TrackOverlayPlugin::cleanup()
{
    hook_dissociate("playback ready", hook_ready);
    hook_dissociate("info change", hook_changed);
    hook_dissociate("tuple change", hook_changed);
    hook_dissociate("title change", hook_changed);
    hook_dissociate("playback stop", hook_stop);

    delete overlay;
    overlay = nullptr;
}
