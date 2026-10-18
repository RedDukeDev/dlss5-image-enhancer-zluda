#include "main_window.h"
#include "half_float.h"

#include "dlss_cuda.h"

#include <cstdlib>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>

// main.cpp: the way this process ends, see there
void end_this_process(int code);

namespace enhancer {
namespace {

// An image file arrives as eight bits a channel; the network reads half-floats.
// Nothing is scaled on the way in: what the file says is what the network sees.
Image from_qimage(const QImage &source) {
    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    Image image;
    image.width = (unsigned)rgba.width();
    image.height = (unsigned)rgba.height();
    image.pixels.resize((size_t)image.width * image.height * 4);
    for (unsigned y = 0; y < image.height; ++y) {
        const uchar *row = rgba.constScanLine((int)y);
        uint16_t *out = image.pixels.data() + (size_t)y * image.width * 4;
        for (unsigned x = 0; x < image.width * 4; ++x)
            out[x] = float_to_half(row[x] / 255.0f);
    }
    return image;
}

// Back to eight bits for the screen and for a file. Values above one clamp:
// without a tone mapper to guess with, that is the honest thing to do.
QImage to_qimage(const Image &image) {
    if (image.empty()) return {};
    QImage out((int)image.width, (int)image.height, QImage::Format_RGBA8888);
    for (unsigned y = 0; y < image.height; ++y) {
        uchar *row = out.scanLine((int)y);
        const uint16_t *in = image.pixels.data() + (size_t)y * image.width * 4;
        for (unsigned x = 0; x < image.width * 4; ++x) {
            float v = half_to_float(in[x]);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            row[x] = (uchar)(v * 255.0f + 0.5f);
        }
    }
    return out;
}

QDoubleSpinBox *strength_row(QFormLayout *form, const QString &label, double value,
                             const QString &tip, QWidget *parent) {
    auto *box = new QDoubleSpinBox(parent);
    box->setRange(0.0, 2.0);
    box->setSingleStep(0.05);
    box->setDecimals(2);
    box->setValue(value);
    form->addRow(label, box);
    // On the label too: the text is as much the thing pointed at as the box.
    if (!tip.isEmpty()) {
        box->setToolTip(tip);
        if (QWidget *name = form->labelForField(box)) name->setToolTip(tip);
    }
    return box;
}

// Native separators: Qt's file dialog answers with forward slashes, and those
// travel on into LoadLibrary, which documents backslashes as the only separator
// it supports.
std::wstring to_wide(const QString &text) { return QDir::toNativeSeparators(text).toStdWString(); }

} // namespace

int run_self_test(const QStringList &arguments) {
    if (arguments.size() < 6) {
        fprintf(stderr, "usage: --selftest <in> <out.png> <network> <driver> [runtime] [nvapi]\n");
        return 2;
    }
    QImage source;
    if (!source.load(arguments[1])) {
        fprintf(stderr, "the input image could not be read\n");
        return 1;
    }
    const Image input = from_qimage(source);
    fprintf(stderr, "input %ux%u\n", input.width, input.height);

    Paths paths;
    paths.snippet = to_wide(arguments[3]);
    // The self-test names every file explicitly, which is what the overrides in
    // Paths are for; an empty argument leaves the ordinary resolution in place.
    paths.cuda_driver = to_wide(arguments[4]);
    paths.nvidia = paths.cuda_driver.empty();
    if (arguments.size() > 5) paths.ngx_runtime = to_wide(arguments[5]);
    if (arguments.size() > 6) paths.nvapi = to_wide(arguments[6]);

    Processor processor;
    std::string error;
    if (!processor.start(paths, error, [](const std::string &line) {
            fprintf(stderr, "%s\n", line.c_str());
        })) {
        fprintf(stderr, "start: %s\n", error.c_str());
        return 1;
    }
    Image output;
    if (!processor.process(input, output, Settings{}, error)) {
        fprintf(stderr, "process: %s\n", error.c_str());
        return 1;
    }
    fprintf(stderr, "processed in %.0f ms\n", processor.last_ms());
    if (!to_qimage(output).save(arguments[2])) {
        fprintf(stderr, "the result could not be written\n");
        return 1;
    }
    fprintf(stderr, "written to %s\n", arguments[2].toUtf8().constData());
    return 0;
}

namespace {
// Forwards a precompile progress line to the log window, in front of its own
// definition -- see below, next to log_to_window which it shares its plumbing
// with.
void log_to_window(const char *line);
} // namespace

void Worker::start(const Paths &paths) {
    std::string error;
    const bool ok = processor_.start(
        paths, error, [](const std::string &line) { log_to_window(line.c_str()); });
    emit started(ok, QString::fromStdString(error));
}

void Worker::process(const Image &input, const Settings &settings) {
    Image output;
    std::string error;
    const bool ok = processor_.process(input, output, settings, error);
    emit finished(ok, output, processor_.last_ms(), QString::fromStdString(error));
}

namespace {
// The DLSS layer takes a plain function, so the window it belongs to is reached
// through this. Only one window exists, and it outlives the layer.
MainWindow *g_window = nullptr;

void log_to_window(const char *line) {
    if (!g_window || !line) return;
    // The layer reports from whichever thread is working, so the line is queued
    // rather than touching a widget from the wrong one.
    QMetaObject::invokeMethod(g_window, "append_log", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromUtf8(line)));
}
} // namespace

MainWindow::MainWindow() {
    setWindowTitle(tr("DLSS 5 Image Enhancer"));
    setAcceptDrops(true);
    resize(1280, 800);

    view_ = new QLabel(tr("Drop an image here, or use Open image"));
    view_->setAlignment(Qt::AlignCenter);
    view_->setMinimumSize(160, 90);
    // Ignored in both directions on purpose: the label holds a pixmap scaled to
    // whatever room the window has, and a policy that respected the pixmap's
    // size would let the window grow to fit it and never shrink back.
    view_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    view_->setStyleSheet("QLabel { background: #202020; color: #a0a0a0; }");

    auto *central = new QWidget;
    auto *layout = new QHBoxLayout(central);
    layout->addWidget(view_, 1);
    layout->addWidget(build_controls());
    setCentralWidget(central);

    status_ = new QLabel(tr("Idle"));
    statusBar()->addWidget(status_);

    worker_ = new Worker;
    worker_->moveToThread(&worker_thread_);
    connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &Worker::started, this, &MainWindow::on_started);
    connect(worker_, &Worker::finished, this, &MainWindow::on_finished);
    worker_thread_.start();

    restore_mode();

    g_window = this;
    dlss_cuda::set_log_sink(&log_to_window);
}

MainWindow::~MainWindow() {
    dlss_cuda::set_log_sink(nullptr);
    g_window = nullptr;
    remember_mode();
    worker_thread_.quit();
    // A translation or an evaluation in progress on the worker thread can run
    // for minutes with no way to interrupt it partway through -- precompile's
    // wait on its child processes has no cancellation of its own. Waiting on
    // the thread here without a bound would mean the window is gone from the
    // screen but the process, and every child still under it, lingers for just
    // as long.
    //
    // A bounded wait, then a hard exit rather than a hang: what mattered has
    // already happened above (the log sink is detached, the paths are saved),
    // and main.cpp's job object takes the still-running children down with it
    // the moment this process actually ends -- which a hard exit forces now
    // instead of leaving to however long the translation was going to take.
    if (!worker_thread_.wait(2000)) {
        ::end_this_process(0);
    }
}

QWidget *MainWindow::build_controls() {
    auto *panel = new QWidget;
    panel->setFixedWidth(420);
    auto *layout = new QVBoxLayout(panel);

    layout->addWidget(build_paths());

    auto *network = new QGroupBox(tr("Network"));
    auto *form = new QFormLayout(network);

    intensity_ = strength_row(
        form, tr("Overall Intensity"), 1.0,
        tr("How strongly the effect is applied. 1 is full strength, 0 is none.\n\n"
           "Below 1 the picture can also come out darker: raise Passes to bring "
           "the brightness back."),
        panel);
    // The network takes no more than 1: a larger value is read as 1.
    intensity_->setMaximum(1.0);
    local_tone_ = strength_row(
        form, tr("Local Tone Intensity"), 1.0,
        tr("Changes the lighting and contrast of each area of the picture.\n\n"
           "1 is the normal amount. Lower values soften it, higher values push "
           "it further."),
        panel);
    local_structure_ = strength_row(
        form, tr("Structure Intensity"), 1.0,
        tr("Adds fine detail and texture to surfaces, fabric and foliage.\n\n"
           "1 is the normal amount. Lower values soften it, higher values push "
           "it further."),
        panel);
    // The network's own default for the skin is "automatic": it follows the
    // structure strength. The box comes first and says so; the number below
    // takes over when it is unticked.
    skin_auto_ = new QCheckBox(tr("Automatic (follows Structure Intensity)"), panel);
    skin_auto_->setChecked(true);
    const QString skin_tip =
        tr("How much detail is added to faces and skin.\n\n"
           "Tick Automatic to use the same amount as Structure Intensity, or "
           "untick it and choose your own value below.\n\n"
           "It needs Automatic mask to be on.");
    skin_auto_->setToolTip(skin_tip);
    form->addRow(tr("Character / Skin Structure"), skin_auto_);
    if (QWidget *name = form->labelForField(skin_auto_)) name->setToolTip(skin_tip);
    skin_structure_ = strength_row(form, QString(), 1.0, skin_tip, panel);
    skin_structure_->setEnabled(false);
    connect(skin_auto_, &QCheckBox::toggled, skin_structure_,
            [this](bool automatic) { skin_structure_->setEnabled(!automatic); });

    style_ = new QComboBox(panel);
    style_->addItems({tr("Default"), tr("Natural"), tr("Cinematic")});
    const QString style_tip =
        tr("Gives the picture a different look.\n\n"
           "Default leaves the look as it is. Natural and Cinematic each change "
           "the tones in their own way.");
    style_->setToolTip(style_tip);
    form->addRow(tr("NR Style"), style_);
    if (QWidget *name = form->labelForField(style_)) name->setToolTip(style_tip);

    preset_ = new QComboBox(panel);
    preset_->addItems({tr("Library default"), tr("Preset 1 (larger)"), tr("Preset 2 (smaller, faster)")});
    const QString preset_tip =
        tr("Which network the library runs.\n\n"
           "Library default is the one NVIDIA's library picks. Preset 1 is the larger "
           "network the earlier versions of the library carried: slower. Preset 2 is "
           "the smaller one. Versions of the library with a single network ignore this.");
    preset_->setToolTip(preset_tip);
    form->addRow(tr("NR Network"), preset_);
    if (QWidget *name = form->labelForField(preset_)) name->setToolTip(preset_tip);

    auto_mask_ = new QCheckBox(tr("Automatic mask"), panel);
    auto_mask_->setChecked(true);
    auto_mask_->setToolTip(
        tr("Lets the program find faces and skin by itself.\n\n"
           "If you turn it off, the skin setting stops working."));
    form->addRow(QString(), auto_mask_);

    passes_ = new QSpinBox(panel);
    passes_->setRange(1, 16);
    passes_->setValue(1);
    const QString passes_tip =
        tr("How many times the effect is applied, one on top of the other: each "
           "pass works on the result of the one before.\n\n"
           "More passes give a stronger effect, but too many make the picture look "
           "artificial. Each pass takes as long as the first.");
    passes_->setToolTip(passes_tip);
    keep_tone_ = new QCheckBox(tr("Keep local tone"), panel);
    keep_tone_->setChecked(true);
    keep_tone_->setEnabled(false);
    keep_tone_->setToolTip(
        tr("Applies Local Tone Intensity on every pass.\n\n"
           "Because the passes stack, the lighting is pushed further each time. "
           "Untick it to apply the local tone only on the first pass."));
    connect(passes_, qOverload<int>(&QSpinBox::valueChanged), keep_tone_,
            [this](int passes) { keep_tone_->setEnabled(passes > 1); });
    auto *passes_row = new QWidget(panel);
    auto *passes_layout = new QHBoxLayout(passes_row);
    passes_layout->setContentsMargins(0, 0, 0, 0);
    passes_layout->addWidget(passes_);
    passes_layout->addWidget(keep_tone_);
    passes_layout->addStretch(1);
    form->addRow(tr("Passes"), passes_row);
    if (QWidget *name = form->labelForField(passes_row)) name->setToolTip(passes_tip);

    layout->addWidget(network);

    auto *buttons = new QWidget(panel);
    auto *row = new QHBoxLayout(buttons);
    row->setContentsMargins(0, 0, 0, 0);
    auto *open = new QPushButton(tr("Open image..."), buttons);
    run_button_ = new QPushButton(tr("Enhance"), buttons);
    save_button_ = new QPushButton(tr("Save result..."), buttons);
    run_button_->setEnabled(false);
    save_button_->setEnabled(false);
    row->addWidget(open);
    row->addWidget(run_button_);
    row->addWidget(save_button_);
    layout->addWidget(buttons);

    compare_button_ = new QPushButton(tr("Hold to see the original"), panel);
    compare_button_->setEnabled(false);
    layout->addWidget(compare_button_);

    // What the DLSS layer and the network have to say. Without this the
    // messages go to standard error, which a windowed program does not have,
    // and a refusal arrives as a code with no explanation behind it.
    log_ = new QPlainTextEdit(panel);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(500);
    log_->setPlaceholderText(tr("Messages from the DLSS layer appear here"));
    layout->addWidget(log_, 1);

    connect(open, &QPushButton::clicked, this, &MainWindow::choose_image);
    connect(run_button_, &QPushButton::clicked, this, &MainWindow::run);
    connect(save_button_, &QPushButton::clicked, this, &MainWindow::save_result);
    connect(compare_button_, &QPushButton::pressed, this, [this] { show_original(true); });
    connect(compare_button_, &QPushButton::released, this, [this] { show_original(false); });
    return panel;
}

QWidget *MainWindow::build_paths() {
    auto *box = new QGroupBox(tr("Mode"));
    auto *layout = new QVBoxLayout(box);

    // A two-state button rather than a combo box or a pair of radio buttons:
    // there are exactly two ways to run this, switching between them is the
    // only thing it does, and its own label already says which one is active.
    mode_button_ = new QPushButton(tr("Mode: AMD (ZLUDA)"), box);
    mode_button_->setCheckable(true);
    layout->addWidget(mode_button_);

    connect(mode_button_, &QPushButton::toggled, this, &MainWindow::set_nvidia_mode);
    return box;
}

// The mode only decides which CUDA driver and which NVAPI Processor::start
// takes; there is nothing to fill in. The NGX runtime is ours in both modes:
// the snippet only accepts calls that appear to come from a module named
// nvngx.dll, by name and not by what the module can do, and a real NVIDIA
// nvngx.dll answers to that name without exporting the calls this makes.
void MainWindow::set_nvidia_mode(bool nvidia) {
    nvidia_mode_ = nvidia;
    mode_button_->setText(nvidia ? tr("Mode: NVIDIA") : tr("Mode: AMD (ZLUDA)"));
}

void MainWindow::restore_mode() {
    QSettings settings("dlss5-image-enhancer", "paths");
    // Called directly rather than through the button's toggled signal: a
    // checkable QPushButton does not emit it when setChecked is given the
    // state it already has.
    const bool nvidia = settings.value("nvidia_mode", false).toBool();
    mode_button_->setChecked(nvidia);
    set_nvidia_mode(nvidia);
}

void MainWindow::remember_mode() const {
    QSettings settings("dlss5-image-enhancer", "paths");
    settings.setValue("nvidia_mode", nvidia_mode_);
}

Settings MainWindow::current_settings() const {
    Settings settings;
    settings.intensity = (float)intensity_->value();
    settings.local_tone = (float)local_tone_->value();
    settings.local_structure = (float)local_structure_->value();
    // -1 is the network's "automatic": it follows the structure strength.
    settings.skin_structure = skin_auto_->isChecked() ? -1.0f : (float)skin_structure_->value();
    settings.style = style_->currentIndex();
    settings.preset = preset_->currentIndex();
    settings.auto_mask = auto_mask_->isChecked();
    settings.passes = passes_->value();
    settings.keep_local_tone = keep_tone_->isChecked();
    return settings;
}

void MainWindow::choose_image() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open image"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
    if (!path.isEmpty()) load_image(path);
}

void MainWindow::load_image(const QString &path) {
    QImage image;
    if (!image.load(path)) {
        QMessageBox::warning(this, tr("Open image"), tr("That file could not be read."));
        return;
    }
    input_ = from_qimage(image);
    have_output_ = false;
    output_ = {};
    showing_output_ = false;
    display(input_);
    run_button_->setEnabled(true);
    save_button_->setEnabled(false);
    compare_button_->setEnabled(false);
    status_->setText(tr("Loaded %1 x %2").arg(input_.width).arg(input_.height));
}

void MainWindow::display(const Image &image) {
    const QImage shown = to_qimage(image);
    if (shown.isNull()) return;
    // Kept at full size: the pixmap on screen is a scaled copy, and rescaling
    // from the original on every resize avoids compounding the loss.
    shown_ = QPixmap::fromImage(shown);
    rescale();
}

void MainWindow::rescale() {
    if (shown_.isNull()) return;
    view_->setPixmap(shown_.scaled(view_->size(), Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    rescale();
}

void MainWindow::append_log(const QString &line) {
    if (!log_) return;
    log_->appendPlainText(line.trimmed());
}

void MainWindow::show_original(bool original) {
    if (!have_output_) return;
    showing_output_ = !original;
    display(original ? input_ : output_);
}

void MainWindow::set_busy(bool busy) {
    run_button_->setEnabled(!busy && !input_.empty());
    save_button_->setEnabled(!busy && have_output_);
    compare_button_->setEnabled(!busy && have_output_);
}

void MainWindow::run() {
    if (input_.empty()) return;
    // The network ships beside this program. In NVIDIA mode it is never opened by
    // path at all: only its folder is handed to NGX as a search path, and NGX
    // then looks there for "nvngx_dlssnr.dll" itself.
    const QString network =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("nvngx_dlssnr.dll"));
    if (!QFileInfo::exists(network)) {
        QMessageBox::warning(this, tr("Enhance"),
                             tr("The network (nvngx_dlssnr.dll) was not found beside the program:\n%1")
                                 .arg(QDir::toNativeSeparators(network)));
        return;
    }
    set_busy(true);
    // The first run on a machine translates the network's code, which with a
    // cold cache takes tens of minutes. Saying so beats looking frozen.
    status_->setText(tr("Working. The very first run on this machine has to translate the "
                        "network and can take a long time."));

    Paths paths;
    paths.snippet = to_wide(network);
    paths.nvidia = nvidia_mode_;

    QMetaObject::invokeMethod(worker_, "start", Qt::QueuedConnection, Q_ARG(Paths, paths));
}

void MainWindow::on_started(bool ok, const QString &message) {
    if (!ok) {
        set_busy(false);
        status_->setText(tr("Failed to start"));
        QMessageBox::critical(this, tr("Enhance"), message);
        return;
    }
    QMetaObject::invokeMethod(worker_, "process", Qt::QueuedConnection, Q_ARG(Image, input_),
                              Q_ARG(Settings, current_settings()));
}

void MainWindow::on_finished(bool ok, const Image &output, double milliseconds,
                             const QString &message) {
    if (!ok) {
        set_busy(false);
        status_->setText(tr("Failed"));
        QMessageBox::critical(this, tr("Enhance"), message);
        return;
    }
    output_ = output;
    have_output_ = true;
    showing_output_ = true;
    display(output_);
    set_busy(false);
    status_->setText(tr("Done in %1 ms").arg((qint64)milliseconds));
}

void MainWindow::save_result() {
    if (!have_output_) return;
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Save result"), "enhanced.png", tr("PNG (*.png)"));
    if (path.isEmpty()) return;
    if (!to_qimage(output_).save(path))
        QMessageBox::warning(this, tr("Save result"), tr("That file could not be written."));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty()) load_image(urls.first().toLocalFile());
}

} // namespace enhancer
