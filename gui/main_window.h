// The window: an image on the left, the network's controls on the right.
//
// The controls are the ones the ReShade addon exposes, because they are the
// ones the network actually reads. Nothing here decides anything about DLSS;
// it collects settings and hands them to the processor.

#pragma once

#include <QMainWindow>
#include <QPixmap>
#include <QStringList>
#include <QString>
#include <QThread>

#include "../core/image_processor.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QSlider;
class QPlainTextEdit;

namespace enhancer {

// Runs the processor away from the interface thread. An evaluation takes
// seconds and the first one can take far longer, so doing it in place would
// freeze the window and look like a hang.
class Worker : public QObject {
    Q_OBJECT
public:
    Worker() = default;

public slots:
    void start(const Paths &paths);
    void process(const Image &input, const Settings &settings);

signals:
    void started(bool ok, const QString &message);
    void finished(bool ok, const Image &output, double milliseconds, const QString &message);

private:
    Processor processor_;
};

// Runs the same path the window runs -- file in, conversion, processor,
// conversion, file out -- with no window at all. It exists so that path can be
// verified: a graphical program that can only be tested by clicking is a
// program whose failures are found by the person using it.
int run_self_test(const QStringList &arguments);

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

protected:
    // Dropping an image on the window is the shortest way in, so it is
    // supported alongside the button.
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    // The picture is scaled to whatever room the window currently has, so
    // resizing has to redraw it.
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void choose_image();
    void save_result();
    void run();
    void on_started(bool ok, const QString &message);
    void on_finished(bool ok, const Image &output, double milliseconds, const QString &message);
    void show_original(bool original);
    // Toggled by the AMD/NVIDIA button. On NVIDIA the driver, the NGX runtime
    // and NVAPI are never picked by hand -- they are found the normal way, by
    // the network and by dlss_cuda.cpp's own fallback to the bare DLL name --
    // so their rows are hidden and their fields forced empty; on AMD they
    // behave exactly as they always have.
    void set_nvidia_mode(bool nvidia);

private:
    QWidget *build_controls();
    QWidget *build_paths();
    void load_image(const QString &path);
    void display(const Image &image);
    void rescale();
    void set_busy(bool busy);
    void remember_paths() const;
    void restore_paths();
    Settings current_settings() const;

    QThread worker_thread_;
    Worker *worker_ = nullptr;

    QLabel *view_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *run_button_ = nullptr;
    QPushButton *save_button_ = nullptr;
    QPushButton *compare_button_ = nullptr;

    QPushButton *mode_button_ = nullptr;
    bool nvidia_mode_ = false;

    QLineEdit *snippet_path_ = nullptr;
    QLineEdit *driver_path_ = nullptr;
    QLineEdit *runtime_path_ = nullptr;
    QLineEdit *nvapi_path_ = nullptr;
    // The driver/nvapi rows, hidden as a whole in NVIDIA mode -- the NGX
    // runtime row is never hidden, see set_nvidia_mode. Each is the field
    // widget passed to QFormLayout::addRow, which is also the key
    // QFormLayout::labelForField needs to reach the row's other half.
    QWidget *driver_row_ = nullptr;
    QWidget *nvapi_row_ = nullptr;

    QDoubleSpinBox *intensity_ = nullptr;
    QDoubleSpinBox *global_tone_ = nullptr;
    QDoubleSpinBox *local_tone_ = nullptr;
    QDoubleSpinBox *local_structure_ = nullptr;
    QDoubleSpinBox *skin_structure_ = nullptr;
    QComboBox *style_ = nullptr;
    QComboBox *preset_ = nullptr;
    QCheckBox *auto_mask_ = nullptr;
    QSpinBox *passes_ = nullptr;

    Image input_;
    Image output_;
    bool have_output_ = false;
    // Which of the two is on screen, so a resize can redraw the right one.
    bool showing_output_ = false;
    // The picture at full size; what the label holds is a scaled copy.
    QPixmap shown_;

public slots:
    // The DLSS layer reports through a plain callback; this carries its lines
    // to the window, which is the only place a user can see them.
    void append_log(const QString &line);
};

} // namespace enhancer

// Declared here rather than in the entry point: the code Qt generates for these
// signals lives beside this header, and it has to know the types before it can
// carry one across a thread.
Q_DECLARE_METATYPE(enhancer::Image)
Q_DECLARE_METATYPE(enhancer::Settings)
Q_DECLARE_METATYPE(enhancer::Paths)
