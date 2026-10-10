#include "Recorder.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QRegularExpression>
#include <QtEndian>
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <windows.h>
#include <shlobj.h>

namespace speaker {
namespace {
constexpr uint64_t SplitFrames = 48000ull * 60 * 60 * 3;
constexpr size_t QueueFrames = 48000 * 4;
QByteArray header(uint64_t frames) {
 QByteArray h(44, '\0');
 std::memcpy(h.data(), "RIFF", 4); std::memcpy(h.data()+8,"WAVEfmt ",8);
 auto u32=[&](int p, quint32 n){qToLittleEndian(n,reinterpret_cast<uchar*>(h.data()+p));};
 auto u16=[&](int p, quint16 n){qToLittleEndian(n,reinterpret_cast<uchar*>(h.data()+p));};
 u32(4,quint32(36+frames*4)); u32(16,16); u16(20,1);u16(22,2);
 u32(24,48000);u32(28,192000);u16(32,4);u16(34,16);
 std::memcpy(h.data()+36,"data",4);u32(40,quint32(frames*4)); return h;
}
QString uniquePath(const QString& folder, const QString& stem) {
 for (int n=0;;++n) {
  auto p=QDir(folder).filePath(stem+(n?QString("_%1").arg(n):QString())+".wav");
  if(!QFileInfo::exists(p)&&!QFileInfo::exists(p+".part")&&!QFileInfo::exists(p+".part.journal")) return p;
 }
}
struct Entry { Recording r; bool pending=false; QString finalPath,recoveryOriginal; };
}
struct Recorder::Impl {
 std::array<float,QueueFrames*2> queue{};
 std::atomic<uint64_t> head{0},tail{0},total{0};
 std::atomic<bool> accepting{false},overflow{false};
 std::atomic<unsigned> producers{0};
 std::thread worker;
 mutable std::mutex mutex;
 std::vector<Entry> entries;
 QString message,error,indexPath;
 bool indexHealthy=true;
 Impl() {
  auto dir=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  QDir().mkpath(dir); indexPath=QDir(dir).filePath("recordings-v1.json");
  QFile f(indexPath);
  if(f.exists()&&!f.open(QIODevice::ReadOnly)) { error="녹음 목록을 읽을 수 없습니다: "+f.errorString();indexHealthy=false;return; }
  if(!f.isOpen()) return;
  QJsonParseError parse;
  auto doc=QJsonDocument::fromJson(f.readAll(),&parse);
  if(parse.error!=QJsonParseError::NoError||doc.object()["format"].toString()!="speaker-recordings-v1") {
   error="녹음 목록이 손상되어 안전하게 불러올 수 없습니다.";indexHealthy=false;return;
  }
  for(auto v:doc.object()["entries"].toArray()) {
   auto j=v.toObject(); Entry e;
   e.r.id=j["id"].toString();e.r.path=j["path"].toString();e.finalPath=j["final"].toString();e.recoveryOriginal=j["recoveryOriginal"].toString();
   e.r.date=QDateTime::fromString(j["date"].toString(),Qt::ISODateWithMs);
   e.r.frames=j["frames"].toString().toULongLong();e.r.bytes=j["bytes"].toString().toULongLong();e.pending=j["pending"].toBool();
   if(!e.r.id.isEmpty()&&QFileInfo(e.r.path).isAbsolute()) entries.push_back(e);
  }
 }
 Entry& entry(const QString& id) { return *std::find_if(entries.begin(),entries.end(),[&](const Entry& e){return e.r.id==id;}); }
 bool save() {
  if(!indexHealthy)return false;
  QJsonArray a;
  for(const auto& e:entries) a.append(QJsonObject{{"id",e.r.id},{"path",e.r.path},{"final",e.finalPath},{"recoveryOriginal",e.recoveryOriginal},{"date",e.r.date.toString(Qt::ISODateWithMs)},
   {"frames",QString::number(e.r.frames)},{"bytes",QString::number(e.r.bytes)},{"pending",e.pending}});
  QSaveFile f(indexPath); auto bytes=QJsonDocument(QJsonObject{{"format","speaker-recordings-v1"},{"entries",a}}).toJson();
  if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit()) {
   error="녹음 목록 저장 실패: "+f.errorString();return false;
  } return true;
 }
 void fail(const QString& s) { accepting=false;std::lock_guard lock(mutex);error=s; }
 bool openSegment(QFile& file,const QString& folder,QString& idx) {
  Entry e; e.r.id=QUuid::createUuid().toString(QUuid::WithoutBraces);e.r.date=QDateTime::currentDateTime();
  e.finalPath=uniquePath(folder,"Speaker_"+e.r.date.toString("yyyy-MM-dd_HH-mm-ss"));
  e.r.path=e.finalPath+".part"; e.r.active=true;e.pending=true;
  file.setFileName(e.r.path);
  if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)) {fail("녹음 파일 생성 실패: "+file.errorString());return false;}
  if(file.write(header(0))!=44||!file.flush()){fail("WAV 헤더 저장 실패: "+file.errorString());file.close();return false;}
  QSaveFile journal(e.r.path+".journal");
  auto bytes=QJsonDocument(QJsonObject{{"format","speaker-pcm-v1"},{"id",e.r.id},{"path",e.r.path},{"final",e.finalPath}}).toJson();
  if(!journal.open(QIODevice::WriteOnly)||journal.write(bytes)!=bytes.size()||!journal.commit()){
   fail("복구 기록을 저장할 수 없어 녹음을 시작하지 않았습니다.");file.close();return false;
  }
  std::lock_guard lock(mutex);idx=e.r.id;entries.push_back(e);
  if(!save()){entry(idx).r.active=false;file.close();return false;} return true;
 }
 bool finish(QFile& file,QString idx,uint64_t frames) {
  if(!file.isOpen()) return false;
  if(!file.seek(0)||file.write(header(frames))!=44||!file.flush()){
   fail("녹음 마무리 실패. 다음 실행 시 복구를 확인하세요: "+file.errorString());file.close();
   std::lock_guard lock(mutex);entry(idx).r.active=false;return false;
  }
  file.close();
  std::lock_guard lock(mutex); auto& e=entry(idx);e.r.active=false;e.r.frames=frames;e.r.bytes=44+frames*4;
  if(!QFile::rename(e.r.path,e.finalPath)){error="파일 확정 실패. 파일 잠금을 해제한 뒤 앱을 다시 실행하여 복구하세요.";save();return false;}
  auto journal=e.r.path+".journal"; e.r.path=e.finalPath;e.pending=false;
  if(!save())return false;
  QFile::remove(journal);message="녹음 저장 완료: "+e.r.path;return true;
 }
 void run(QString folder,QFile* initial,QString idx) {
  std::unique_ptr<QFile> file(initial);uint64_t segment=0,sinceFlush=0;
  std::array<char,4096*4> pcm{};
  while(true){
   uint64_t t=tail.load(std::memory_order_relaxed),h=head.load(std::memory_order_acquire);
   if(t==h){
    if(!accepting.load()&&producers.load()==0&&head.load(std::memory_order_acquire)==t)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;
   }
   size_t n=size_t(std::min<uint64_t>({h-t,4096,SplitFrames-segment}));
   for(size_t i=0;i<n;++i)for(size_t c=0;c<2;++c){
    float sample=queue[((t+i)%QueueFrames)*2+c]; if(!std::isfinite(sample))sample=0;
    sample=std::clamp(sample,-1.f,1.f);
    auto value=qint16(std::lrint(sample*(sample<0?32768.f:32767.f)));
    qToLittleEndian(value,reinterpret_cast<uchar*>(pcm.data()+(i*2+c)*2));
   }
   auto written=file->write(pcm.data(),qint64(n*4));
   if(written!=qint64(n*4)){
    // Keep only complete PCM frames; retain the temporary file and journal on failure.
    if(written>0)segment+=uint64_t(written)/4;
    file->resize(44+qint64(segment*4));file->flush();file->close();
    fail("녹음 쓰기 실패(저장 공간/장치 확인). 복구용 임시 파일을 보존했습니다.");break;
   }
   segment+=n;sinceFlush+=n;total.fetch_add(n);tail.store(t+n,std::memory_order_release);
   if(sinceFlush>=48000){sinceFlush=0;if(!file->flush()){auto reason=file->errorString();file->close();fail("녹음 디스크 쓰기 실패. 복구용 임시 파일을 보존했습니다: "+reason);break;}}
   if(segment==SplitFrames){
    if(!finish(*file,idx,segment)){accepting=false;break;}
    // Open the next segment only when another sample is ready, avoiding empty tail files.
    while(head.load()==tail.load()&&(accepting.load()||producers.load()!=0))std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if(head.load()==tail.load()&&!accepting.load())break;
    if(!openSegment(*file,folder,idx)){accepting=false;break;}segment=0;
   }
  }
  if(file->isOpen())finish(*file,idx,segment);
  std::lock_guard lock(mutex);for(auto& e:entries)e.r.active=false;
  if(overflow.load())error="저장 큐가 가득 차 녹음을 중단했습니다. 저장된 구간을 확인하고 더 빠른 저장 장치를 사용하세요.";
  accepting=false;
 }
};
Recorder::Recorder():d(std::make_unique<Impl>()){}
Recorder::~Recorder(){stop();}
bool Recorder::start(const QString& folder,QString& error){
 if(d->accepting.load()){error="이미 녹음 중입니다.";return false;}stop();
 if(folder.isEmpty()||!QFileInfo(folder).isAbsolute()||!QDir().mkpath(folder)){error="저장 폴더를 만들 수 없습니다.";return false;}
 {std::lock_guard lock(d->mutex);if(!d->indexHealthy){error=d->error;return false;}d->error.clear();d->message.clear();}
 d->head=0;d->tail=0;d->total=0;d->overflow=false;
 auto file=std::make_unique<QFile>();QString idx;
 if(!d->openSegment(*file,folder,idx)){error=status().error;return false;}
 d->accepting=true;
 try{d->worker=std::thread([this,folder,f=std::move(file),idx]() mutable {d->run(folder,f.release(),idx);});}
 catch(const std::exception& ex){d->accepting=false;error="녹음 저장 스레드 생성 실패: "+QString::fromUtf8(ex.what());std::lock_guard lock(d->mutex);d->entry(idx).r.active=false;d->error=error;return false;}
 return true;
}
void Recorder::push(const float* stereo,size_t frames) noexcept {
 d->producers.fetch_add(1,std::memory_order_acq_rel);
 if(d->accepting.load(std::memory_order_acquire)&&stereo&&frames){
  auto h=d->head.load(std::memory_order_relaxed),t=d->tail.load(std::memory_order_acquire);
  if(frames>QueueFrames-(h-t)){d->overflow=true;d->accepting=false;}
  else{for(size_t i=0;i<frames;++i){d->queue[((h+i)%QueueFrames)*2]=stereo[i*2];d->queue[((h+i)%QueueFrames)*2+1]=stereo[i*2+1];}d->head.store(h+frames,std::memory_order_release);}
 }
 d->producers.fetch_sub(1,std::memory_order_release);
}
void Recorder::stop(){d->accepting=false;if(d->worker.joinable())d->worker.join();}
RecorderStatus Recorder::status()const{std::lock_guard lock(d->mutex);return {d->accepting.load(),double(d->total.load())/48000,d->message,d->error};}
std::vector<Recording> Recorder::recordings()const{
 std::lock_guard lock(d->mutex);std::vector<Recording> result;
 for(const auto& e:d->entries){auto r=e.r;r.recoveryRequired=e.pending&&!r.active;QFileInfo f(r.path);r.missing=!f.exists();if(!r.missing){r.bytes=uint64_t(f.size());if(r.active)r.frames=r.bytes>=44?(r.bytes-44)/4:0;}result.push_back(r);}return result;
}
bool Recorder::rename(const QString& id,const QString& name,QString& error){
 QString n=name; if(!n.endsWith(".wav",Qt::CaseInsensitive))n+=".wav";
 auto stem=n.left(n.size()-4);
 static const QRegularExpression forbidden("[<>:\"/\\\\|?*\\x00-\\x1f]");
 static const QRegularExpression reserved("^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\\.|$)",QRegularExpression::CaseInsensitiveOption);
 if(stem.trimmed().isEmpty()||name.endsWith(' ')||name.endsWith('.')||stem.endsWith(' ')||stem.endsWith('.')||n.size()>240||forbidden.match(n).hasMatch()||reserved.match(n).hasMatch()){
  error="Windows에서 사용할 수 없는 이름입니다. 금지 문자, 예약 이름, 끝의 공백/마침표를 확인하세요.";return false;
 }
 std::lock_guard lock(d->mutex);
 for(auto& e:d->entries)if(e.r.id==id){
  if(e.r.active||e.pending){error="녹음 중이거나 복구가 필요한 파일은 이름을 변경할 수 없습니다.";return false;}
  auto target=QDir(QFileInfo(e.r.path).absolutePath()).filePath(n);
  if(target==e.r.path)return true;
  if(QFileInfo::exists(target)){error="같은 이름의 파일이 이미 있습니다.";return false;}
  auto old=e.r.path;
  if(!QFile::rename(old,target)){error="이름 변경 실패. 파일이 있는지 확인하고 사용 중인 플레이어를 닫은 뒤 다시 시도하세요.";return false;}
  e.r.path=target;e.finalPath=target;
  if(!d->save()){if(QFile::rename(target,old)){e.r.path=old;e.finalPath=old;}error=d->error;return false;}return true;
 } error="목록에서 파일을 찾지 못했습니다.";return false;
}
bool Recorder::trash(const QString& id,QString& error){
 std::lock_guard lock(d->mutex);
 for(auto i=d->entries.begin();i!=d->entries.end();++i)if(i->r.id==id){
  if(i->r.active||i->pending){error="녹음 중이거나 복구가 필요한 파일은 삭제할 수 없습니다.";return false;}
  QFile file(i->r.path);
  if(!file.moveToTrash()){error="휴지통으로 이동하지 못했습니다. 파일 사용/저장 장치를 확인하세요. 영구 삭제하지 않았습니다. "+file.errorString();return false;}
  d->entries.erase(i);if(!d->save()){error=d->error;return false;}return true;
 }error="목록에서 파일을 찾지 못했습니다.";return false;
}
void Recorder::cleanMissing(){std::lock_guard lock(d->mutex);std::erase_if(d->entries,[](const Entry& e){return !e.r.active&&!e.pending&&!QFileInfo::exists(e.r.path);});d->save();}
QString Recorder::recover(){
 std::lock_guard lock(d->mutex);int count=0,failed=0;
 for(auto& e:d->entries){
  if(!e.pending||e.r.active)continue;
  QFile journal(e.r.path+".journal");
  if(!journal.open(QIODevice::ReadOnly)){++failed;continue;}
  auto j=QJsonDocument::fromJson(journal.readAll()).object();
  if(j["format"].toString()!="speaker-pcm-v1"||j["id"].toString()!=e.r.id||j["path"].toString()!=e.r.path||j["final"].toString()!=e.finalPath||e.r.path!=e.finalPath+".part") {++failed;continue;}
  QFile original(e.r.path);
  // A crash after final rename but before catalog commit leaves a valid finalized WAV.
  if(!original.exists()&&QFileInfo::exists(e.finalPath)){
   original.setFileName(e.finalPath);
   if(original.open(QIODevice::ReadOnly)&&original.size()>=44&&uint64_t(original.size()-44)/4<=SplitFrames&&original.size()%4==0&&original.read(44)==header(uint64_t(original.size()-44)/4)){
    e.r.path=e.finalPath;e.pending=false;e.r.frames=uint64_t(original.size()-44)/4;e.r.bytes=uint64_t(original.size());++count;
   }else ++failed;continue;
  }
  if(!original.open(QIODevice::ReadOnly)||original.size()<48||original.size()>qint64(44+SplitFrames*4)){++failed;continue;}
  auto h=original.read(44),expected=header(0);
  // Only the two length fields may differ in an interrupted application-owned PCM WAV.
  h.replace(4,4,expected.mid(4,4));h.replace(40,4,expected.mid(40,4));
  if(h!=expected){++failed;continue;}
  auto frames=uint64_t(original.size()-44)/4;
  auto target=uniquePath(QFileInfo(e.finalPath).absolutePath(),QFileInfo(e.finalPath).completeBaseName()+"_복구");
  QSaveFile out(target);bool ok=out.open(QIODevice::WriteOnly)&&out.write(header(frames))==44;
  uint64_t left=frames*4;
  while(ok&&left){auto chunk=original.read(qint64(std::min<uint64_t>(left,1024*1024)));if(chunk.isEmpty()||out.write(chunk)!=chunk.size()){ok=false;break;}left-=uint64_t(chunk.size());}
  if(!ok||!out.commit()){++failed;continue;}
  e.recoveryOriginal=e.r.path;e.r.path=target;e.finalPath=target;e.r.frames=frames;e.r.bytes=44+frames*4;e.pending=false;++count;
 }
 if(count&&!d->save())return "파일은 복구했지만 목록 저장에 실패했습니다: "+d->error;
 if(!count&&!failed)return {};
 return QString("녹음 %1개 복구, %2개 복구 불가. 비정상 종료 직전의 소리가 손실되었을 수 있습니다. 복구 원본 .part와 .journal은 원래 위치에 보존했습니다.").arg(count).arg(failed);
}
QString Recorder::defaultFolder(){PWSTR p=nullptr;QString root;
 if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music,KF_FLAG_DEFAULT,nullptr,&p))){root=QString::fromWCharArray(p);CoTaskMemFree(p);}
 if(root.isEmpty())root=QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
 return QDir(root).filePath("Speaker Recordings");
}
}




