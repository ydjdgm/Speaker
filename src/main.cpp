#include <QApplication>
#include <QLocale>
#include <QDir>
#include <QLibraryInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTranslator>
#include "MainWindow.h"
int main(int argc,char* argv[]) {
 QApplication app(argc,argv);
 QCoreApplication::setOrganizationName("SpeakerLocal");
 QCoreApplication::setApplicationName("Speaker");
 QCoreApplication::setApplicationVersion("1.0.0");
 QLocale::setDefault(QLocale(QLocale::Korean,QLocale::SouthKorea));
 QTranslator translator;
 if(translator.load("qtbase_ko",QLibraryInfo::path(QLibraryInfo::TranslationsPath)))app.installTranslator(&translator);
 const QString data=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
 if(!QDir().mkpath(data)){QMessageBox::critical(nullptr,"Speaker 시작 실패","앱 설정 폴더를 만들 수 없습니다. Windows 계정의 저장 권한을 확인하세요.");return 1;}
 QLockFile instance(QDir(data).filePath("speaker-instance.lock"));instance.setStaleLockTime(0);
 if(!instance.tryLock(0)){QMessageBox::information(nullptr,"Speaker","이미 실행 중이거나 앱 설정 폴더에 접근할 수 없습니다. 기존 Speaker 창을 확인하세요.");return 1;}
 speaker::MainWindow window;window.show();return app.exec();
}
