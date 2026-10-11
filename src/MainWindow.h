#pragma once
#include <QMainWindow>
#include <QSettings>
#include "AudioEngine.h"
#include "Recorder.h"
class QComboBox;
class QCheckBox;
class QSlider;
class QLabel;
class QProgressBar;
class QTableWidget;
class QPushButton;
class QCloseEvent;
class QSystemTrayIcon;
namespace speaker {
class MainWindow final : public QMainWindow {
public:
 MainWindow();
 ~MainWindow() override;
protected:
 void closeEvent(QCloseEvent*) override;
private:
 Recorder recorder_;
 AudioEngine engine_;
 AudioConfig config_;
 QSettings settings_;
 QString folder_;
 std::vector<Program> programs_;
 std::vector<Recording> recordings_;
 QComboBox *microphone_, *program_, *monitorDevice_, *cable_;
 QCheckBox *mute_, *share_, *monitor_, *transmit_, *record_, *stable_;
 QSlider *microphoneGain_, *programGain_, *monitorGain_;
 QLabel *microphoneState_, *programState_, *monitorState_, *transmitState_, *recordState_, *folderLabel_, *notice_, *diagnostics_;
 QProgressBar *microphoneMeter_, *programMeter_, *mixMeter_;
 QTableWidget* files_;
 bool updating_=true;
 QSystemTrayIcon* tray_=nullptr;
 bool trayNoticeShown_=false;
 bool exitRequested_=false;
 void setupTray();
 void restoreWindow();
 void quitApplication();
 QString lastRecorderError_;
 void buildUi();
 void refreshDevices();
 void refreshPrograms();
 void refreshFiles();
 void apply();
 void poll();
 void saveSettings();
 void setSwitches();
 bool hasSource() const;
 void message(const QString&, bool error=false);
 QString selectedRecording() const;
 void renameRecording();
 void trashRecording();
 void revealRecording();
 void chooseFolder();
 void showGuide();
 bool configureEngine(QString& error);
};
}
