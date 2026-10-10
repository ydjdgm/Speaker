#include "MainWindow.h"
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace speaker {
namespace {
QLabel* label(const QString& text) {
 auto* item = new QLabel(text); item->setWordWrap(true); item->setTextFormat(Qt::PlainText);
 item->setTextInteractionFlags(Qt::TextSelectableByMouse); return item;
}
QProgressBar* meter(const QString& name) {
 auto* bar=new QProgressBar; bar->setRange(0,100); bar->setValue(0); bar->setFormat("무음 · %p%");
 bar->setAccessibleName(name); bar->setMaximumHeight(22); return bar;
}
QSlider* gain(const QString& name, int value) {
 auto* slider=new QSlider(Qt::Horizontal); slider->setRange(0,150); slider->setValue(value);
 slider->setAccessibleName(name); slider->setTickInterval(25); slider->setSingleStep(5); return slider;
}
QWidget* gainRow(QSlider* slider) {
 auto* row=new QWidget; auto* layout=new QHBoxLayout(row); layout->setContentsMargins(0,0,0,0);
 auto* value=new QLabel(QString::number(slider->value())+"%"); value->setMinimumWidth(48);
 layout->addWidget(slider); layout->addWidget(value);
 QObject::connect(slider,&QSlider::valueChanged,value,[value](int v){value->setText(QString::number(v)+"%");}); return row;
}
QComboBox* choices(const QString& name) {
 auto* box=new QComboBox; box->setAccessibleName(name); box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
 box->setMinimumContentsLength(24); return box;
}
void populate(QComboBox* box, const std::vector<Device>& devices, const QString& saved, bool cableOnly) {
 QSignalBlocker block(box); box->clear(); box->addItem("선택 안 함",QString());
 for(const auto& device:devices) if(device.cable==cableOnly){box->addItem(device.name,device.id);box->setItemData(box->count()-1,true,Qt::UserRole+1);}
 int index=box->findData(saved);
 if(index<0 && !saved.isEmpty()){box->addItem("연결되지 않은 저장 장치 — 다시 선택하세요",saved); index=box->count()-1;}
 box->setCurrentIndex(std::max(0,index));
}
QString duration(double seconds) {
 const auto s=static_cast<qint64>(seconds);
 return QString("%1:%2:%3").arg(s/3600,2,10,QChar('0')).arg((s/60)%60,2,10,QChar('0')).arg(s%60,2,10,QChar('0'));
}
void updateMeter(QProgressBar* bar,float peak) {
 int value=static_cast<int>(std::clamp(peak,0.0f,1.0f)*100);
 bar->setValue(value); bar->setFormat(value ? "입력 있음 · %p%" : "무음 · %p%");
}
bool confirm(QWidget* parent,const QString& title,const QString& text) {
 QMessageBox box(QMessageBox::Question,title,text,QMessageBox::Yes|QMessageBox::No,parent);
 box.button(QMessageBox::Yes)->setText("예");box.button(QMessageBox::No)->setText("아니요");
 box.setDefaultButton(QMessageBox::No);return box.exec()==QMessageBox::Yes;
}
void logAudioFault(const QString& states) {
 const QString directory=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
 if(!QDir().mkpath(directory))return;
 const QString path=QDir(directory).filePath("audio-errors.log");
 if(QFileInfo(path).size()>256*1024){QFile::remove(path+".1");if(!QFile::rename(path,path+".1"))return;}
 QFile file(path);if(file.open(QIODevice::Append|QIODevice::Text))file.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate)+" "+states+"\n").toUtf8());
}
}
MainWindow::MainWindow() {
 setWindowTitle("Speaker — 로컬 오디오 공유 및 녹음"); resize(880,900); setMinimumSize(620,540);
 config_.microphoneId=settings_.value("devices/microphone").toString();
 config_.monitorId=settings_.value("devices/monitor").toString();
 config_.cableId=settings_.value("devices/cable").toString();
 config_.microphoneGain=std::clamp(settings_.value("gain/microphone",1.0).toFloat(),0.0f,1.5f);
 config_.programGain=std::clamp(settings_.value("gain/program",1.0).toFloat(),0.0f,1.5f);
 config_.monitorGain=std::clamp(settings_.value("gain/monitor",.7).toFloat(),0.0f,1.5f);
 config_.muted=settings_.value("microphone/muted",false).toBool();
 config_.stable=settings_.value("audio/stable",false).toBool();
 folder_=settings_.value("recordings/folder",Recorder::defaultFolder()).toString();
 if(folder_.isEmpty()) folder_=Recorder::defaultFolder();
 buildUi(); refreshDevices(); refreshPrograms(); setSwitches(); updating_=false;
 engine_.setRecordSink([this](const float* frames,size_t count){recorder_.push(frames,count);});
 const QString recovery=recorder_.recover(); refreshFiles();
 auto* timer=new QTimer(this); connect(timer,&QTimer::timeout,this,[this]{poll();}); timer->start(100);
 auto* listTimer=new QTimer(this); connect(listTimer,&QTimer::timeout,this,[this]{refreshFiles();}); listTimer->start(3000);
 if(!recovery.isEmpty()) QTimer::singleShot(0,this,[this,recovery]{QMessageBox::information(this,"이전 녹음 복구 결과",recovery+"\n비정상 종료 직전의 소리는 일부 손실되었을 수 있습니다.");});
 if(!settings_.value("guide/shown",false).toBool()) QTimer::singleShot(0,this,[this]{showGuide();settings_.setValue("guide/shown",true);});
 message("시작 준비 완료 · 전송 / 프로그램 공유 / 내 목소리 듣기 / 녹음은 모두 OFF입니다.");
}
MainWindow::~MainWindow(){engine_.stop(); recorder_.stop(); saveSettings();}
void MainWindow::buildUi() {
 auto* central=new QWidget; auto* outer=new QVBoxLayout(central); setCentralWidget(central);
 auto* title=label("Speaker  |  내 마이크 + 선택한 프로그램"); QFont font=title->font();font.setPointSize(font.pointSize()+3);font.setBold(true);title->setFont(font);outer->addWidget(title);
 notice_=label(""); notice_->setAccessibleName("알림 및 오류"); outer->addWidget(notice_);
 auto* tabs=new QTabWidget; outer->addWidget(tabs);
 auto* mixer=new QWidget; auto* mainLayout=new QVBoxLayout(mixer);
 auto* refresh=new QPushButton("장치 / 프로그램 새로 고침"); mainLayout->addWidget(refresh);
 connect(refresh,&QPushButton::clicked,this,[this]{refreshDevices();refreshPrograms();message("목록을 새로 고쳤습니다. 연결 해제된 장치는 직접 다시 선택하고 필요한 기능을 켜세요.");});
 auto* micGroup=new QGroupBox("내 마이크"); auto* micLayout=new QFormLayout(micGroup);
 microphone_=choices("마이크 장치"); micLayout->addRow("마이크 장치",microphone_);
 microphoneGain_=gain("마이크 전달 음량",qRound(config_.microphoneGain*100));micLayout->addRow("전달 음량",gainRow(microphoneGain_));
 mute_=new QCheckBox("마이크 전달 음소거 (녹음에도 적용)");micLayout->addRow(mute_);
 microphoneMeter_=meter("마이크 입력 레벨");micLayout->addRow("입력 레벨",microphoneMeter_);
 microphoneState_=label("대기 · 활성 기능 없음");micLayout->addRow(microphoneState_);mainLayout->addWidget(micGroup);
 auto* appGroup=new QGroupBox("공유할 프로그램");auto* appLayout=new QFormLayout(appGroup);
 program_=choices("공유할 프로그램");appLayout->addRow("프로그램",program_);
 share_=new QCheckBox("프로그램 공유 ON");appLayout->addRow(share_);
 programGain_=gain("프로그램 전달 음량",qRound(config_.programGain*100));appLayout->addRow("전달 음량",gainRow(programGain_));
 programMeter_=meter("프로그램 입력 레벨");appLayout->addRow("입력 레벨",programMeter_);
 appLayout->addRow(label("선택한 프로그램과 하위 프로세스의 소리를 포함합니다. 같은 브라우저의 모든 창·탭이 포함될 수 있습니다. 통화 앱은 선택하지 마세요. 유튜브와 통화는 다른 브라우저 또는 별도 통화 앱을 사용하세요."));
 programState_=label("공유 OFF");appLayout->addRow(programState_);mainLayout->addWidget(appGroup);
 auto* monitorGroup=new QGroupBox("내 목소리 듣기");auto* monitorLayout=new QFormLayout(monitorGroup);
 monitorDevice_=choices("이어폰 또는 스피커");monitorLayout->addRow("이어폰",monitorDevice_);
 monitor_=new QCheckBox("내 목소리 듣기 ON");monitorLayout->addRow(monitor_);
 monitorGain_=gain("모니터링 음량",qRound(config_.monitorGain*100));monitorLayout->addRow("듣기 음량",gainRow(monitorGain_));
 monitorLayout->addRow(label("USB 마이크만 추가로 들립니다. 전달 음소거·전달 음량과 독립적입니다. Bluetooth는 지연될 수 있으며 유선 이어폰과 비교할 수 있습니다."));
 monitorState_=label("모니터링 OFF");monitorLayout->addRow(monitorState_);mainLayout->addWidget(monitorGroup);
 auto* sendGroup=new QGroupBox("전송과 녹음 — 각각 독립적으로 제어");auto* sendLayout=new QFormLayout(sendGroup);
 cable_=choices("가상 마이크 재생 장치");sendLayout->addRow("CABLE Input",cable_);
 transmit_=new QCheckBox("전송 ON → 가상 마이크");sendLayout->addRow(transmit_);
 transmitState_=label("전송 OFF");sendLayout->addRow(transmitState_);
 mixMeter_=meter("최종 혼합 출력 레벨");sendLayout->addRow("혼합 레벨",mixMeter_);
 record_=new QCheckBox("녹음 ON → WAV 파일");sendLayout->addRow(record_);
 recordState_=label("녹음 OFF · 00:00:00");sendLayout->addRow(recordState_);
 sendLayout->addRow(label("녹음은 전달 직전 혼합 소리입니다. 전송 OFF에서도 녹음할 수 있습니다. 통화 앱의 후속 음성 처리는 포함하지 않습니다."));mainLayout->addWidget(sendGroup); mainLayout->addStretch();
 auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setWidget(mixer);tabs->addTab(scroll,"오디오 공유");
 auto* library=new QWidget;auto* libraryLayout=new QVBoxLayout(library);
 folderLabel_=label("저장 폴더: "+QDir::toNativeSeparators(folder_));libraryLayout->addWidget(folderLabel_);
 auto* folderButtons=new QHBoxLayout;auto* choose=new QPushButton("저장 위치 변경");auto* openFolder=new QPushButton("저장 폴더 열기");folderButtons->addWidget(choose);folderButtons->addWidget(openFolder);libraryLayout->addLayout(folderButtons);
 connect(choose,&QPushButton::clicked,this,[this]{chooseFolder();});connect(openFolder,&QPushButton::clicked,this,[this]{if(!QDir().mkpath(folder_)||!QDesktopServices::openUrl(QUrl::fromLocalFile(folder_)))message("저장 폴더를 열 수 없습니다. 경로와 권한을 확인하세요.",true);});
 libraryLayout->addWidget(label("새 저장 위치는 다음 녹음부터 적용됩니다. 기존 파일은 이동하지 않습니다. 아래 목록은 Speaker가 관리하는 녹음만 표시합니다."));
 files_=new QTableWidget(0,5);files_->setHorizontalHeaderLabels({"파일명","녹음 날짜","길이","크기","상태"});files_->setSelectionBehavior(QAbstractItemView::SelectRows);files_->setSelectionMode(QAbstractItemView::SingleSelection);files_->setEditTriggers(QAbstractItemView::NoEditTriggers);files_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);files_->setAccessibleName("녹음 목록");libraryLayout->addWidget(files_);
 auto* fileButtons=new QHBoxLayout;auto* rename=new QPushButton("이름 변경");auto* trash=new QPushButton("휴지통으로 삭제");auto* reveal=new QPushButton("탐색기에서 보기");auto* clean=new QPushButton("없는 항목 정리");
 for(auto* button:{rename,trash,reveal,clean})fileButtons->addWidget(button);libraryLayout->addLayout(fileButtons);
 connect(rename,&QPushButton::clicked,this,[this]{renameRecording();});connect(trash,&QPushButton::clicked,this,[this]{trashRecording();});connect(reveal,&QPushButton::clicked,this,[this]{revealRecording();});connect(clean,&QPushButton::clicked,this,[this]{recorder_.cleanMissing();refreshFiles();message("외부에서 삭제 또는 이동되어 찾을 수 없는 항목을 목록에서 정리했습니다.");});
 libraryLayout->addWidget(label("녹음 확인: 전송을 끄고 탐색기에서 기존 오디오 플레이어로 여세요. 그 플레이어나 상위 프로세스가 공유 대상으로 선택되어 있으면 다시 전달될 수 있습니다."));tabs->addTab(library,"녹음 목록");
 auto* settingsPage=new QWidget;auto* settingsLayout=new QVBoxLayout(settingsPage);
 auto* guide=new QPushButton("가상 마이크 연결 안내");settingsLayout->addWidget(guide);connect(guide,&QPushButton::clicked,this,[this]{showGuide();});
 stable_=new QCheckBox("안정성 우선 (버퍼 증가로 지연이 늘어날 수 있음)");stable_->setChecked(config_.stable);settingsLayout->addWidget(stable_);
 settingsLayout->addWidget(label("설정 변경은 활성 오디오 경로를 잠시 재구성할 수 있습니다. Windows 기본 장치·통신 장치는 변경하지 않습니다. 분리된 장치는 자동 대체하거나 자동 재개하지 않습니다."));
 diagnostics_=label("");diagnostics_->setAccessibleName("오디오 진단");settingsLayout->addWidget(diagnostics_);
 settingsLayout->addWidget(label("내부 혼합: 48 kHz float32 스테레오 / 저장: 48 kHz 16-bit PCM 스테레오 WAV / 3시간 자동 분할\n내부 지연 20 ms 이하는 설계 목표이며 측정된 성능 보장이 아닙니다. 진단의 버퍼 추정치는 Bluetooth·드라이버·네트워크를 포함하는 종단 지연이 아닙니다.\nDRM 콘텐츠, 통화 앱의 노이즈 억제·자동 감도, Bluetooth 프로필 전환과 장치 드라이버는 결과에 영향을 줄 수 있습니다.\n오디오는 로컬에서 처리합니다. 계정, 클라우드 업로드, 텔레메트리가 없습니다."));settingsLayout->addStretch();tabs->addTab(settingsPage,"설정 및 진단");
 for(auto* box:{microphone_,monitorDevice_,cable_})connect(box,&QComboBox::currentIndexChanged,this,[this]{apply();});
 connect(program_,&QComboBox::currentIndexChanged,this,[this]{apply();});
 for(auto* check:{mute_,share_,monitor_,transmit_,record_,stable_})connect(check,&QCheckBox::toggled,this,[this]{apply();});
 for(auto* slider:{microphoneGain_,programGain_,monitorGain_})connect(slider,&QSlider::valueChanged,this,[this]{apply();});
}
void MainWindow::refreshDevices(){
 populate(microphone_,AudioEngine::inputDevices(),config_.microphoneId,false);
 const auto outputs=AudioEngine::outputDevices();populate(monitorDevice_,outputs,config_.monitorId,false);populate(cable_,outputs,config_.cableId,true);
}
void MainWindow::refreshPrograms(){
 const Program previous=config_.program; QSignalBlocker block(program_);program_->clear();program_->addItem("직접 선택하세요 — 자동 공유하지 않음");programs_=AudioEngine::programs();int selected=0;
 const QString lastPath=settings_.value("program/lastPath").toString();
 for(size_t i=0;i<programs_.size();++i){const auto& p=programs_[i];program_->addItem(p.icon,p.name+(p.path==lastPath&&!lastPath.isEmpty()?" (이전 선택 후보)":""));program_->setItemData(static_cast<int>(i)+1,p.path,Qt::ToolTipRole);if(p.pid==previous.pid&&p.created==previous.created)selected=static_cast<int>(i)+1;}
 program_->setCurrentIndex(selected);
 if(!updating_ && previous.pid && !selected){config_.program={};config_.share=false;setSwitches();apply();}
}
void MainWindow::setSwitches(){
 const QSignalBlocker a(mute_),b(share_),c(monitor_),d(transmit_),e(record_);
 mute_->setChecked(config_.muted);share_->setChecked(config_.share);monitor_->setChecked(config_.monitor);transmit_->setChecked(config_.transmit);record_->setChecked(config_.recording);
}
bool MainWindow::hasSource() const {return (!config_.microphoneId.isEmpty()&&microphone_->currentData(Qt::UserRole+1).toBool()&&!config_.muted&&config_.microphoneGain>0)||(config_.share&&config_.program.pid&&config_.programGain>0);}
void MainWindow::apply(){
 if(updating_)return;
 const AudioConfig previous=config_;
 config_.microphoneId=microphone_->currentData().toString();config_.monitorId=monitorDevice_->currentData().toString();config_.cableId=cable_->currentData().toString();
 const int programIndex=program_->currentIndex()-1;config_.program=programIndex>=0&&programIndex<static_cast<int>(programs_.size())?programs_[programIndex]:Program{};
 config_.microphoneGain=microphoneGain_->value()/100.f;config_.programGain=programGain_->value()/100.f;config_.monitorGain=monitorGain_->value()/100.f;
 config_.muted=mute_->isChecked();config_.share=share_->isChecked();config_.monitor=monitor_->isChecked();config_.transmit=transmit_->isChecked();config_.recording=record_->isChecked();config_.stable=stable_->isChecked();
 QString error;
 if(config_.share&&!config_.program.pid){config_.share=false;error="공유할 프로그램을 먼저 선택하세요.";}
 if(config_.monitor&&(config_.microphoneId.isEmpty()||config_.monitorId.isEmpty())){config_.monitor=false;error="내 목소리를 들으려면 마이크와 실제 이어폰을 선택하세요.";}
 if(config_.transmit&&config_.cableId.isEmpty()){config_.transmit=false;error="전송하려면 VB-CABLE의 CABLE Input을 선택하세요. 설정 탭에서 설치 안내를 볼 수 있습니다.";}
 if(!hasSource()){
  if(config_.transmit&&!previous.transmit){config_.transmit=false;error="전송을 시작할 소스가 없습니다. 마이크 음소거를 해제하거나 프로그램 공유를 켜고 전달 음량을 올리세요.";}
  if(config_.recording&&!previous.recording){config_.recording=false;error="녹음할 소스가 없습니다. 마이크를 선택하거나 프로그램 공유를 켜고 전달 음량을 올리세요.";}
 }
 if(config_.recording&&!previous.recording){lastRecorderError_.clear();if(!recorder_.start(folder_,error))config_.recording=false;}
 if(!config_.recording&&previous.recording)recorder_.stop();
 QString engineError;
 if(!configureEngine(engineError)){
  if(!engineError.isEmpty())error=engineError;
  if(config_.recording&&!previous.recording)recorder_.stop();
  config_=previous;
  // A stopped recorder cannot be resumed implicitly after a rejected change.
  config_.recording=recorder_.status().recording;
  updating_=true;
  refreshDevices();
  int selected=0;for(size_t i=0;i<programs_.size();++i)if(programs_[i].pid==config_.program.pid&&programs_[i].created==config_.program.created)selected=static_cast<int>(i)+1;
  program_->setCurrentIndex(selected);
  microphoneGain_->setValue(qRound(config_.microphoneGain*100));programGain_->setValue(qRound(config_.programGain*100));monitorGain_->setValue(qRound(config_.monitorGain*100));stable_->setChecked(config_.stable);
  updating_=false;
  QString ignored;configureEngine(ignored);
 }
 setSwitches();saveSettings();if(!error.isEmpty())message(error,true);
 if(previous.recording!=config_.recording)refreshFiles();
}
void MainWindow::poll(){
 const auto s=engine_.status();const auto r=recorder_.status();
 microphoneState_->setText((s.microphoneActive?QString("마이크 입력 활성 · "):QString("마이크 입력 대기 · "))+s.microphoneState);
 programState_->setText((s.programActive?QString("프로그램 공유 중 · "):config_.share?QString("공유 선택됨 / 전송·녹음 대기 · "):QString("프로그램 공유 OFF · "))+s.programState);
 monitorState_->setText((s.monitorActive?QString("내 목소리 듣기 ON · "):QString("모니터링 OFF · "))+s.monitorState);
 transmitState_->setText((s.transmitActive?QString("전송 중 · "):QString("전송 OFF · "))+(config_.cableId.isEmpty()?QString("VB-CABLE을 선택하세요 · "):QString("CABLE Input 선택됨 · "))+s.transmitState);
 updateMeter(microphoneMeter_,s.microphonePeak);updateMeter(programMeter_,s.programPeak);updateMeter(mixMeter_,s.mixPeak);
 if(s.clipped)mixMeter_->setFormat("피크 보호 작동 · %p%");
 recordState_->setText(QString("%1 · %2\n%3").arg(r.recording?"● 녹음 중":"녹음 OFF",duration(r.seconds),r.error.isEmpty()?r.message:r.error));
 diagnostics_->setText(QString("실제 오디오 형식\n%1\n\n추정 내부 큐 / 버퍼 지연: %2 ms\n오버런: %3  |  언더런: %4\n선택 프로그램 PID: %5\n프로그램 실행 경로: %6\n피크 보호: %7").arg(s.formats).arg(s.queueMilliseconds,0,'f',1).arg(s.overruns).arg(s.underruns).arg(config_.program.pid).arg(config_.program.path).arg(s.clipped?"작동":"대기"));
 QStringList faults;
 const bool current=s.generation==config_.generation;
 if(current&&s.microphoneFault&&!config_.microphoneId.isEmpty()){
  config_.microphoneId.clear();config_.monitor=false;const QSignalBlocker block(microphone_);microphone_->setCurrentIndex(0);
  faults<<s.microphoneState;
 }
 if(current&&s.programFault&&config_.share){config_.share=false;faults<<s.programState;}
 if(current&&s.transmitFault&&config_.transmit){config_.transmit=false;faults<<s.transmitState;}
 if(current&&s.monitorFault&&config_.monitor){config_.monitor=false;faults<<s.monitorState;}
 if(!faults.isEmpty()){
  setSwitches();QString ignored;configureEngine(ignored);logAudioFault(faults.join(" | "));
  message(faults.join("\n")+"\n관련 경로를 중지했습니다. 목록 새로 고침 후 장치를 다시 선택하거나 기능을 직접 켜세요."+(config_.recording?" 녹음은 남은 입력 또는 무음을 계속 저장합니다.":""),true);
 }
 if(config_.recording&&!r.recording){config_.recording=false;setSwitches();QString error;configureEngine(error);refreshFiles();}
 if(!r.error.isEmpty()&&r.error!=lastRecorderError_){lastRecorderError_=r.error;message("녹음 중지: "+r.error+" 전송과 모니터링은 독립적으로 유지됩니다.",true);}
}
bool MainWindow::configureEngine(QString& error){++config_.generation;return engine_.configure(config_,error);}
void MainWindow::saveSettings(){
 settings_.setValue("devices/microphone",config_.microphoneId);settings_.setValue("devices/monitor",config_.monitorId);settings_.setValue("devices/cable",config_.cableId);
 settings_.setValue("gain/microphone",config_.microphoneGain);settings_.setValue("gain/program",config_.programGain);settings_.setValue("gain/monitor",config_.monitorGain);settings_.setValue("microphone/muted",config_.muted);settings_.setValue("audio/stable",config_.stable);settings_.setValue("recordings/folder",folder_);
 if(!config_.program.path.isEmpty())settings_.setValue("program/lastPath",config_.program.path);
}
void MainWindow::message(const QString& text,bool error){notice_->setText((error?"오류 / 확인 필요: ":"")+text);statusBar()->showMessage(text);}
QString MainWindow::selectedRecording()const {const int row=files_->currentRow();return row>=0&&files_->item(row,0)?files_->item(row,0)->data(Qt::UserRole).toString():QString();}
void MainWindow::refreshFiles(){
 const QString selected=selectedRecording();recordings_=recorder_.recordings();files_->setRowCount(static_cast<int>(recordings_.size()));
 for(size_t i=0;i<recordings_.size();++i){const auto& r=recordings_[i];const QStringList values={QFileInfo(r.path).fileName(),r.date.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"),duration(r.frames/48000.0),QString::number(r.bytes/1048576.0,'f',2)+" MB",r.active?"녹음 중":r.missing?"찾을 수 없음":r.recoveryRequired?"복구 필요 — 앱 재시작":"저장됨"};
  for(int column=0;column<5;++column){auto* item=new QTableWidgetItem(values[column]);item->setToolTip(QDir::toNativeSeparators(r.path));if(column==0)item->setData(Qt::UserRole,r.id);files_->setItem(static_cast<int>(i),column,item);}if(r.id==selected)files_->selectRow(static_cast<int>(i));
 }
}
void MainWindow::renameRecording(){
 const QString id=selectedRecording();auto it=std::find_if(recordings_.begin(),recordings_.end(),[&](const Recording& r){return r.id==id;});if(it==recordings_.end()){message("이름을 바꿀 녹음을 먼저 선택하세요.",true);return;}
 if(it->active||it->missing){message("녹음 중이거나 찾을 수 없는 파일은 이름을 바꿀 수 없습니다.",true);return;}
 bool ok=false;QString name=QInputDialog::getText(this,"녹음 이름 변경","새 파일명 (확장자 .wav는 자동 적용)",QLineEdit::Normal,QFileInfo(it->path).completeBaseName(),&ok);if(!ok)return;QString error;if(!recorder_.rename(id,name,error))message(error+" 파일을 사용하는 플레이어가 있으면 닫고 다시 시도하세요.",true);else message("녹음 이름을 변경했습니다.");refreshFiles();
}
void MainWindow::trashRecording(){
 const QString id=selectedRecording();auto it=std::find_if(recordings_.begin(),recordings_.end(),[&](const Recording& r){return r.id==id;});if(it==recordings_.end()){message("삭제할 녹음을 먼저 선택하세요.",true);return;}
 if(it->active||it->missing){message("녹음 중인 파일은 삭제할 수 없습니다. 없는 파일은 '없는 항목 정리'를 사용하세요.",true);return;}
 if(!confirm(this,"휴지통으로 이동",QFileInfo(it->path).fileName()+"\n이 녹음을 휴지통으로 이동할까요?"))return;
 QString error;if(!recorder_.trash(id,error))message(error+" 파일을 사용하는 플레이어를 닫고 다시 시도하세요. 영구 삭제로 전환하지 않습니다.",true);else message("녹음을 휴지통으로 이동했습니다.");refreshFiles();
}
void MainWindow::revealRecording(){
 const QString id=selectedRecording();auto it=std::find_if(recordings_.begin(),recordings_.end(),[&](const Recording& r){return r.id==id;});if(it==recordings_.end()){message("탐색기에서 볼 녹음을 먼저 선택하세요.",true);return;}
 if(!QFileInfo::exists(it->path)){message("파일을 찾을 수 없습니다. 외부에서 이동되었을 수 있습니다.",true);refreshFiles();return;}
 if(!QProcess::startDetached("explorer.exe",QStringList{"/select,",QDir::toNativeSeparators(it->path)}))message("탐색기를 실행할 수 없습니다.",true);
}
void MainWindow::chooseFolder(){const QString selected=QFileDialog::getExistingDirectory(this,"다음 녹음부터 사용할 저장 폴더",folder_);if(selected.isEmpty())return;folder_=selected;folderLabel_->setText("저장 폴더: "+QDir::toNativeSeparators(folder_));saveSettings();message("다음 녹음은 새 폴더에 저장합니다. 진행 중인 녹음과 기존 파일은 원래 위치를 유지합니다.");}
void MainWindow::showGuide(){
 QMessageBox box(this);box.setWindowTitle("VB-CABLE 연결 안내");box.setIcon(QMessageBox::Information);
 const auto outputs=AudioEngine::outputDevices();const bool found=std::any_of(outputs.begin(),outputs.end(),[](const Device& d){return d.cable;});
 box.setText((found?QString("VB-CABLE 재생 장치가 감지되었습니다.\n\n"):QString("VB-CABLE이 감지되지 않았습니다. 설치 없이도 모니터링과 로컬 녹음은 가능합니다.\n\n"))+"1. 필요한 경우 공식 사이트에서 VB-CABLE을 설치하세요. 관리자 권한 승인과 재부팅이 필요할 수 있습니다.\n2. Speaker에서 장치 새로 고침 후 CABLE Input을 선택하세요.\n3. Discord / 게임 입력은 CABLE Output, 출력은 실제 이어폰으로 선택하세요.\n4. 전송 ON을 켜세요. 노이즈 억제와 자동 감도가 음악을 끊으면 통화 앱 설정을 조절하세요.\n\n전송 OFF 또는 앱 종료 후 Speaker의 소리는 전달되지 않습니다. 일반 마이크로 돌아가려면 통화 앱의 입력을 USB 마이크로 다시 선택하세요. Windows 기본 장치는 변경하지 않습니다.");
 auto* official=box.addButton("공식 VB-CABLE 사이트 열기",QMessageBox::ActionRole);box.addButton("닫기",QMessageBox::AcceptRole);box.exec();if(box.clickedButton()==official)QDesktopServices::openUrl(QUrl("https://vb-audio.com/Cable/"));
}
void MainWindow::closeEvent(QCloseEvent* event){
 if(config_.transmit||config_.recording){if(!confirm(this,"Speaker 종료","종료하면 전송과 내 목소리 듣기를 중지하고 녹음 파일을 마무리합니다. 계속하려면 창을 최소화하세요.\n종료할까요?")){event->ignore();return;}}
 engine_.stop();recorder_.stop();saveSettings();event->accept();
}
}
