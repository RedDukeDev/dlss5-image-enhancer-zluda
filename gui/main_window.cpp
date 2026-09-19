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
#include <QLineEdit>
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

QLineEdit *path_row(QFormLayout *form, const QString &label, const QString &filter,
                    QWidget *parent, QWidget **row_out = nullptr) {
    auto *edit = new QLineEdit(parent);
    auto *browse = new QPushButton(QObject::tr("Browse..."), parent);
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(edit, 1);
    layout->addWidget(browse);
    QObject::connect(browse, &QPushButton::clicked, parent, [edit, filter, parent] {
        const QString chosen =
            QFileDialog::getOpenFileName(parent, QObject::tr("Select a library"), edit->text(),
                                         filter);
        if (!chosen.isEmpty()) edit->setText(chosen);
    });
    form->addRow(label, row);
    if (row_out) *row_out = row;
    return edit;
}

QDoubleSpinBox *strength_row(QFormLayout *form, const QString &label, double value,
                             const QString &tip, QWidget *parent) {
    auto *box = new QDoubleSpinBox(parent);
    box->setRange(0.0, 2.0);
    box->setSingleStep(0.05);
    box->setDecimals(2);
    box->setValue(value);
    if (!tip.isEmpty()) box->setToolTip(tip);
    form->addRow(label, box);
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

    restore_paths();

    g_window = this;
    dlss_cuda::set_log_sink(&log_to_window);
}

MainWindow::~MainWindow() {
    dlss_cuda::set_log_sink(nullptr);
    g_window = nullptr;
    remember_paths();
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
        std::_Exit(0);
    }
}

QWidget *MainWindow::build_controls() {
    auto *panel = new QWidget;
    panel->setFixedWidth(420);
    auto *layout = new QVBoxLayout(panel);

    layout->addWidget(build_paths());

    auto *network = new QGroupBox(tr("Network"));
    auto *form = new QFormLayout(network);

    intensity_ = strength_row(form, tr("Overall Intensity"), 1.0, {}, panel);
    global_tone_ = strength_row(
        form, tr("Global Tone Intensity"), 0.0,
        tr("Left at zero. A single run with it at one flattened the picture, but the "
           "evaluation is not reproducible, so that reading cannot be trusted."),
        panel);
    local_tone_ = strength_row(form, tr("Local Tone Intensity"), 1.0, {}, panel);
    local_structure_ = strength_row(form, tr("Structure Intensity"), 1.0, {}, panel);
    skin_structure_ = strength_row(
        form, tr("Character / Skin Structure"), 0.0,
        tr("Left at zero for the same reason as Global Tone above."), panel);

    style_ = new QComboBox(panel);
    style_->addItems({tr("Default"), tr("Natural"), tr("Cinematic")});
    form->addRow(tr("NR Style"), style_);

    preset_ = new QComboBox(panel);
    preset_->addItems({tr("Default"), tr("Preset #1"), tr("Preset #2"), tr("Preset #3")});
    preset_->setToolTip(tr("Which trained weights to use. The network falls back to its "
                           "shipping default when the preset asked for is not in the build."));
    form->addRow(tr("NR Preset"), preset_);

    auto_mask_ = new QCheckBox(tr("Automatic mask"), panel);
    auto_mask_->setChecked(true);
    form->addRow(QString(), auto_mask_);

    passes_ = new QSpinBox(panel);
    passes_->setRange(1, 16);
    passes_->setValue(1);
    passes_->setToolTip(tr("The network blends with its own previous result, which starts "
                           "black. Repeating on a still picture is the nearest thing to a "
                           "scene standing still, and lets the blend settle."));
    form->addRow(tr("Passes"), passes_);

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

namespace {
// Defined further down, next to the search it does.
void fill_in_defaults(QLineEdit *snippet);
} // namespace

QWidget *MainWindow::build_paths() {
    auto *box = new QGroupBox(tr("Libraries"));
    auto *layout = new QVBoxLayout(box);

    // A two-state button rather than a combo box or a pair of radio buttons:
    // there are exactly two ways to run this, switching between them is the
    // only thing it does, and its own label already says which one is active.
    mode_button_ = new QPushButton(tr("Mode: AMD (ZLUDA)"), box);
    mode_button_->setCheckable(true);
    layout->addWidget(mode_button_);

    auto *form_widget = new QWidget(box);
    auto *form = new QFormLayout(form_widget);
    form->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(form_widget);

    const QString dll = tr("Libraries (*.dll)");
    snippet_path_ = path_row(form, tr("Network (nvngx_dlssnr.dll)"), dll, this);

    connect(mode_button_, &QPushButton::toggled, this, &MainWindow::set_nvidia_mode);
    return box;
}

// AMD keeps exactly the behaviour this program has always had: all four
// fields, filled in or picked by hand, held to the checks in run() and in
// Processor::start(). NVIDIA needs only the network and the NGX runtime --
// the driver and NVAPI are found the ordinary way once their fields are empty
// (dlss_cuda.cpp's own fallback to the bare DLL name, and NVAPI left alone
// entirely), so those two rows are hidden and their fields cleared rather
// than left to be filled in and second-guessed.
//
// The NGX runtime is not part of that: it is never the driver's, on either
// GPU. The snippet only accepts calls that appear to come from a module named
// nvngx.dll, and it checks that by name, not by what the module can do -- so
// the loader has to be *our* nvngx.dll (ngx_runtime/ngx_runtime.cpp), the one
// that exports the calls dlss_cuda.cpp actually makes. A real NVIDIA
// nvngx.dll answers to that name but does not export them, and loading it
// fails with "could not load the NGX runtime nvngx.dll" the first time
// something is missing, or subtler errors afterward if it happens to export
// something with the same name by coincidence. So its row stays visible and
// its field filled in the same way in both modes.
void MainWindow::set_nvidia_mode(bool nvidia) {
    nvidia_mode_ = nvidia;
    mode_button_->setText(nvidia ? tr("Mode: NVIDIA") : tr("Mode: AMD (ZLUDA)"));
    fill_in_defaults(snippet_path_);
}

namespace {

// Fills the four paths in when nothing has been chosen yet.
//
// Two arrangements are worth guessing. Beside the executable is where this
// project's own files sit, and they win: the NGX runtime in particular has to be
// ours, since the network refuses to be called from a module by any other name
// and NVIDIA's own nvngx.dll carries none of the exports this needs.
//
// The rest can come from an installed NVIDIA driver: its CUDA driver is in the
// system directory, and the network itself ships in the driver store, where the
// folder name changes with every release and so has to be searched for.
// Finds the network so the user usually does not have to. Beside the
// executable first, then NVIDIA's driver store, which keeps each release in its
// own folder -- so the newest match is the one to take.
//
// Nothing else is guessed here any more: the CUDA driver, NVAPI and the NGX
// runtime are resolved by Processor::start from the mode, and there is no field
// for them to be written into.
void fill_in_defaults(QLineEdit *snippet) {
    if (!snippet->text().isEmpty()) return;
    const QDir beside(QCoreApplication::applicationDirPath());
    QString found = beside.filePath(QStringLiteral("nvngx_dlssnr.dll"));
    if (!QFileInfo::exists(found)) {
        found.clear();
        QDir store("C:/Windows/System32/DriverStore/FileRepository");
        const QStringList folders = store.entryList({"nv_disp*"}, QDir::Dirs, QDir::Time);
        for (const QString &folder : folders) {
            const QString candidate = store.filePath(folder) + "/nvngx_dlssnr.dll";
            if (QFileInfo::exists(candidate)) {
                found = candidate;
                break;
            }
        }
    }
    snippet->setText(found);
}

} // namespace

void MainWindow::restore_paths() {
    QSettings settings("dlss5-image-enhancer", "paths");
    snippet_path_->setText(settings.value("snippet").toString());

    // Applied after the text above, not before: on NVIDIA this clears what was
    // just restored, which is correct there, and on AMD fill_in_defaults only
    // ever touches a field that is still empty, so what was just restored
    // survives untouched.
    //
    // Called directly rather than through the button's toggled signal: a
    // checkable QPushButton does not emit it when setChecked is given the
    // state it already has, and the default unchecked state is exactly the
    // saved value on a first run, where fill_in_defaults still has to run.
    const bool nvidia = settings.value("nvidia_mode", false).toBool();
    mode_button_->setChecked(nvidia);
    set_nvidia_mode(nvidia);
}

void MainWindow::remember_paths() const {
    QSettings settings("dlss5-image-enhancer", "paths");
    settings.setValue("nvidia_mode", nvidia_mode_);
    // Only on AMD: on NVIDIA these three are cleared by design, and saving
    // that over a working AMD configuration would lose it the moment the
    // program happens to close in the other mode.
    settings.setValue("snippet", snippet_path_->text());
}

Settings MainWindow::current_settings() const {
    Settings settings;
    settings.intensity = (float)intensity_->value();
    settings.global_tone = (float)global_tone_->value();
    settings.local_tone = (float)local_tone_->value();
    settings.local_structure = (float)local_structure_->value();
    settings.skin_structure = (float)skin_structure_->value();
    settings.style = style_->currentIndex();
    settings.preset = preset_->currentIndex();
    settings.auto_mask = auto_mask_->isChecked();
    settings.passes = passes_->value();
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
    // The network is the only file the user has to find. Everything else --
    // the CUDA driver, NVAPI, the NGX runtime -- either ships with this program
    // under zluda\ or comes with the NVIDIA driver, and Processor::start picks
    // whichever the mode calls for.
    if (snippet_path_->text().isEmpty()) {
        QMessageBox::warning(this, tr("Enhance"),
                             tr("The network (nvngx_dlssnr.dll) has to be pointed at before "
                                "anything can run."));
        return;
    }
    // The file has to be that one by name, not merely a DLSS snippet. In NVIDIA
    // mode it is never opened by path at all: only its folder is handed to NGX
    // as a search path, and NGX then looks there for "nvngx_dlssnr.dll" itself.
    // Point this at the upscaler (nvngx_dlss.dll) and NGX finds no neural
    // rendering snippet in that folder, falls back to the driver's own copy,
    // which does not carry this feature -- and the only thing the user sees is
    // an opaque 0xBAD0000B from CreateFeature. A renamed copy fails the same
    // way for the same reason, so the name really is the requirement.
    const QFileInfo chosen(snippet_path_->text());
    if (chosen.fileName().compare(QStringLiteral("nvngx_dlssnr.dll"), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this, tr("Enhance"),
            tr("The network has to be the file named nvngx_dlssnr.dll, but this points at "
               "%1.\n\nThat is a different DLSS snippet: nvngx_dlss.dll is the upscaler, and it "
               "carries no neural rendering network. In NVIDIA mode the file is not even opened "
               "by name -- its folder is handed to NGX, which looks there for nvngx_dlssnr.dll "
               "itself, finds nothing, and fails with an error that explains none of this.")
                .arg(chosen.fileName()));
        return;
    }
    set_busy(true);
    // The first run on a machine translates the network's code, which with a
    // cold cache takes tens of minutes. Saying so beats looking frozen.
    status_->setText(tr("Working. The very first run on this machine has to translate the "
                        "network and can take a long time."));

    Paths paths;
    paths.snippet = to_wide(snippet_path_->text());
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
