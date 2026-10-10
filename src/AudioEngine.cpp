#include "AudioEngine.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <tlhelp32.h>
#include <propvarutil.h>
#include <QFileIconProvider>
#include <QFileInfo>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace speaker { namespace {
template<class T> struct Com {
 T* p=nullptr; ~Com(){reset();} Com()=default; Com(const Com&)=delete; Com& operator=(const Com&)=delete;
 T** put(){reset();return &p;} T* operator->()const{return p;} explicit operator bool()const{return p!=nullptr;}
 void reset(){if(p){p->Release();p=nullptr;}}
};
struct Apartment { HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED); ~Apartment(){if(SUCCEEDED(hr))CoUninitialize();} };
QString failure(HRESULT hr){
 if(hr==E_ACCESSDENIED)return QStringLiteral("마이크 접근 거부 — Windows 설정 > 개인정보 및 보안 > 마이크에서 데스크톱 앱 접근을 허용하세요.");
 if(hr==AUDCLNT_E_DEVICE_INVALIDATED)return QStringLiteral("장치 연결 해제 — 새로 고침 후 장치를 다시 선택하세요.");
 if(hr==AUDCLNT_E_UNSUPPORTED_FORMAT)return QStringLiteral("지원하지 않는 오디오 형식 — Windows 장치 형식 설정을 확인하세요.");
 return QStringLiteral("오디오 장치 사용 불가 (0x%1) — 장치 재선택 후 다시 시작하세요.").arg(quint32(hr),8,16,QChar('0'));
}
bool isCable(const QString& name){return name.contains("VB-Audio",Qt::CaseInsensitive)&&name.contains("Cable",Qt::CaseInsensitive);}
std::vector<Device> devices(EDataFlow flow){
 Apartment apartment; Com<IMMDeviceEnumerator> e; std::vector<Device> result;
 if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(e.put()))))return result;
 Com<IMMDeviceCollection> list;if(FAILED(e->EnumAudioEndpoints(flow,DEVICE_STATE_ACTIVE,list.put())))return result;
 UINT count=0;list->GetCount(&count);
 for(UINT i=0;i<count;++i){Com<IMMDevice> dev;Com<IPropertyStore> props;LPWSTR id=nullptr;
  if(FAILED(list->Item(i,dev.put()))||FAILED(dev->GetId(&id)))continue;
  Device d;d.id=QString::fromWCharArray(id);CoTaskMemFree(id);
  if(SUCCEEDED(dev->OpenPropertyStore(STGM_READ,props.put()))){PROPVARIANT v;PropVariantInit(&v);if(SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&v))&&v.vt==VT_LPWSTR)d.name=QString::fromWCharArray(v.pwszVal);PropVariantClear(&v);}
    QString metadata=d.name;
  if(props){for(const PROPERTYKEY* key:{&PKEY_Device_DeviceDesc,&PKEY_DeviceInterface_FriendlyName}){PROPVARIANT v;PropVariantInit(&v);if(SUCCEEDED(props->GetValue(*key,&v))&&v.vt==VT_LPWSTR)metadata+=" "+QString::fromWCharArray(v.pwszVal);PropVariantClear(&v);}}
  d.cable=isCable(metadata);if(flow==eCapture&&d.cable)continue;result.push_back(d);
 }return result;
}
uint64_t creation(HANDLE p){FILETIME c{},e{},k{},u{};if(!GetProcessTimes(p,&c,&e,&k,&u))return 0;return (uint64_t(c.dwHighDateTime)<<32)|c.dwLowDateTime;}
std::unordered_map<DWORD,DWORD> parents(){std::unordered_map<DWORD,DWORD> result;HANDLE h=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(h==INVALID_HANDLE_VALUE)return result;PROCESSENTRY32W p{};p.dwSize=sizeof(p);if(Process32FirstW(h,&p))do{result[p.th32ProcessID]=p.th32ParentProcessID;}while(Process32NextW(h,&p));CloseHandle(h);return result;}
bool unsafe(DWORD pid,const std::unordered_map<DWORD,DWORD>& tree){DWORD current=GetCurrentProcessId();for(size_t n=0;current&&n<=tree.size();++n){if(current==pid)return true;auto it=tree.find(current);if(it==tree.end()||it->second==current)break;current=it->second;}return false;}
bool validProgram(const Program& p){if(!p.pid||unsafe(p.pid,parents()))return false;HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,p.pid);if(!h)return false;bool valid=creation(h)==p.created&&WaitForSingleObject(h,0)==WAIT_TIMEOUT;CloseHandle(h);return valid;}
WAVEFORMATEX mixFormat(){WAVEFORMATEX w{};w.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;w.nChannels=2;w.nSamplesPerSec=48000;w.wBitsPerSample=32;w.nBlockAlign=8;w.nAvgBytesPerSec=384000;return w;}
struct Ring {
 static constexpr size_t capacity=4800;std::array<float,capacity*2> data{};size_t read=0,count=0;bool primed=false;double average=0;
 void clear(){read=count=0;primed=false;average=0;}
 void discard(size_t frames){frames=std::min(frames,count);read=(read+frames)%capacity;count-=frames;}
 void push(const float* p,size_t frames,uint64_t& lost){if(frames>capacity){if(p)p+=(frames-capacity)*2;frames=capacity;++lost;}if(count+frames>capacity){discard(count+frames-capacity);++lost;}
 for(size_t i=0;i<frames;++i){size_t at=((read+count+i)%capacity)*2;data[at]=p?p[i*2]:0;data[at+1]=p?p[i*2+1]:0;}count+=frames;}
 size_t pop(float* out,size_t frames){size_t n=std::min(count,frames);for(size_t i=0;i<n;++i){out[i*2]=data[((read+i)%capacity)*2];out[i*2+1]=data[((read+i)%capacity)*2+1];}std::fill(out+n*2,out+frames*2,0.f);discard(n);return n;}
 size_t popClocked(float* out,size_t frames,size_t prebuffer){
  if(!primed){if(count<prebuffer){std::fill(out,out+frames*2,0.f);return frames;}primed=true;average=double(prebuffer)-double(frames)/2;}
  average=.999*average+.001*double(count);
  const double target=double(prebuffer)-double(frames)/2;
  size_t consumed=frames;
  if(average>target+48&&count>frames)consumed=frames+1;
  else if(average<target-48&&frames>1&&count>=frames-1)consumed=frames-1;
  if(count<consumed){primed=false;return pop(out,frames);}
  // Smooth fractional sample slips correct clock drift. Windows SRC performs
  // the actual device sample-rate and channel-format conversion.
  for(size_t i=0;i<frames;++i){double position=double(i)*double(consumed)/double(frames);size_t a=size_t(position),b=std::min(a+1,consumed-1);float f=float(position-double(a));for(size_t channel=0;channel<2;++channel){float x=data[((read+a)%capacity)*2+channel],y=data[((read+b)%capacity)*2+channel];out[i*2+channel]=x+(y-x)*f;}}
  discard(consumed);return frames;
 }
 // A bounded slip corrector prevents independent hardware clocks accumulating latency.
 void bound(size_t target,uint64_t& lost){if(count>target){discard(count-target);++lost;}}
};
class Activation final:public IActivateAudioInterfaceCompletionHandler,public IAgileObject {
 std::atomic<ULONG> refs{1};
public:
 HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);HRESULT result=E_PENDING;Com<IAudioClient> client;
 ~Activation(){if(done)CloseHandle(done);}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==__uuidof(IUnknown)||iid==__uuidof(IActivateAudioInterfaceCompletionHandler))*out=static_cast<IActivateAudioInterfaceCompletionHandler*>(this);else if(iid==__uuidof(IAgileObject))*out=static_cast<IAgileObject*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;} ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation)override{Com<IUnknown> object;HRESULT activation=E_FAIL;result=operation->GetActivateResult(&activation,object.put());if(SUCCEEDED(result))result=activation;if(SUCCEEDED(result))result=object->QueryInterface(__uuidof(IAudioClient),reinterpret_cast<void**>(client.put()));SetEvent(done);return S_OK;}
};
struct Stream {
 Com<IAudioClient> client;Com<IAudioCaptureClient> capture;Com<IAudioRenderClient> render;HANDLE event=nullptr,process=nullptr;UINT32 capacity=0;bool inputRoute=false;Ring queue;QString description;
 void adopt(Stream& other){close();std::swap(client.p,other.client.p);std::swap(event,other.event);std::swap(process,other.process);std::swap(capacity,other.capacity);inputRoute=other.inputRoute;description=other.description;}
 ~Stream(){close();}
 void close(){if(client)client->Stop();capture.reset();render.reset();client.reset();if(event)CloseHandle(event);event=nullptr;if(process)CloseHandle(process);process=nullptr;capacity=0;queue.clear();description.clear();}
 HRESULT open(const QString& id,bool input,bool stable,const Program* program=nullptr){close();inputRoute=input;HRESULT hr;
  if(program){process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,program->pid);if(!process||creation(process)!=program->created||WaitForSingleObject(process,0)!=WAIT_TIMEOUT)return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
   AUDIOCLIENT_ACTIVATION_PARAMS params{};params.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;params.ProcessLoopbackParams.TargetProcessId=program->pid;params.ProcessLoopbackParams.ProcessLoopbackMode=PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
   PROPVARIANT v{};v.vt=VT_BLOB;v.blob.cbSize=sizeof(params);v.blob.pBlobData=reinterpret_cast<BYTE*>(&params);auto* complete=new Activation;Com<IActivateAudioInterfaceAsyncOperation> operation;
   hr=ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,__uuidof(IAudioClient),&v,complete,operation.put());
   if(SUCCEEDED(hr)){if(WaitForSingleObject(complete->done,5000)!=WAIT_OBJECT_0)hr=HRESULT_FROM_WIN32(ERROR_TIMEOUT);else{hr=complete->result;if(SUCCEEDED(hr)){client.p=complete->client.p;client.p->AddRef();}}}complete->Release();if(FAILED(hr))return hr;
   description=QStringLiteral("프로그램: 48000 Hz float32 스테레오");
  }else{Com<IMMDeviceEnumerator> e;Com<IMMDevice> dev;hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(e.put()));if(FAILED(hr))return hr;
   hr=e->GetDevice(reinterpret_cast<LPCWSTR>(id.utf16()),dev.put());if(FAILED(hr))return hr;hr=dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.put()));if(FAILED(hr))return hr;
   WAVEFORMATEX* native=nullptr;if(SUCCEEDED(client->GetMixFormat(&native))){description=QStringLiteral("장치 %1 Hz / %2채널 / %3비트 → 내부 48000 Hz float32 스테레오 (Windows SRC)").arg(native->nSamplesPerSec).arg(native->nChannels).arg(native->wBitsPerSample);CoTaskMemFree(native);}
  }
  WAVEFORMATEX format=mixFormat();DWORD flags=AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if(program)flags|=AUDCLNT_STREAMFLAGS_LOOPBACK;
  REFERENCE_TIME period=0,minPeriod=0;if(FAILED(client->GetDevicePeriod(&period,&minPeriod)))period=100000;
  // Shared engine chooses its supported period; the stable mode requests additional headroom.
  hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,stable?std::max<REFERENCE_TIME>(period*3,300000):0,0,&format,nullptr);if(FAILED(hr))return hr;
  event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return HRESULT_FROM_WIN32(GetLastError());hr=client->SetEventHandle(event);if(FAILED(hr))return hr;hr=client->GetBufferSize(&capacity);if(FAILED(hr))return hr;
  return S_OK;
 }
 // Service acquisition, buffer access and service Release all stay on the audio thread.
 HRESULT start(){HRESULT hr;
  if(inputRoute)hr=client->GetService(__uuidof(IAudioCaptureClient),reinterpret_cast<void**>(capture.put()));else hr=client->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(render.put()));if(FAILED(hr))return hr;
  if(render){BYTE* out=nullptr;hr=render->GetBuffer(capacity,&out);if(FAILED(hr))return hr;hr=render->ReleaseBuffer(capacity,AUDCLNT_BUFFERFLAGS_SILENT);if(FAILED(hr))return hr;}
  return client->Start();
 }
 HRESULT read(uint64_t& over){UINT32 available=0;unsigned packets=0;HRESULT hr=capture->GetNextPacketSize(&available);while(SUCCEEDED(hr)&&available&&packets++<64){BYTE* data=nullptr;UINT32 frames=0;DWORD flags=0;hr=capture->GetBuffer(&data,&frames,&flags,nullptr,nullptr);if(FAILED(hr))return hr;if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)++over;queue.push(flags&AUDCLNT_BUFFERFLAGS_SILENT?nullptr:reinterpret_cast<float*>(data),frames,over);hr=capture->ReleaseBuffer(frames);if(FAILED(hr))return hr;hr=capture->GetNextPacketSize(&available);}if(available)++over;return hr;}
 HRESULT write(uint64_t& under,uint64_t& over,bool stable){UINT32 padding=0;HRESULT hr=client->GetCurrentPadding(&padding);if(FAILED(hr))return hr;UINT32 frames=capacity-padding;if(!frames)return S_OK;BYTE* bytes=nullptr;hr=render->GetBuffer(frames,&bytes);if(FAILED(hr))return hr;queue.bound(stable?1920:960,over);if(queue.popClocked(reinterpret_cast<float*>(bytes),frames,stable?1440:480)<frames)++under;return render->ReleaseBuffer(frames,0);}
};
float peak(const float* p,size_t n){float v=0;for(size_t i=0;i<n;++i)if(std::isfinite(p[i]))v=std::max(v,std::abs(p[i]));return v;}
}
struct AudioEngine::Impl {
 mutable std::mutex mutex;AudioConfig requested;AudioStatus state;RecordSink sink;uint64_t revision=1;HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);HANDLE applied=CreateEventW(nullptr,FALSE,FALSE,nullptr);std::atomic<uint64_t> appliedRevision{0};std::atomic<bool> quit{false};std::thread thread;
 Impl(){thread=std::thread([this]{run();});}~Impl(){quit=true;SetEvent(wake);if(thread.joinable())thread.join();CloseHandle(wake);CloseHandle(applied);}
 void run(){Apartment apartment;DWORD taskIndex=0;HANDLE priority=AvSetMmThreadCharacteristicsW(L"Pro Audio",&taskIndex);HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);LARGE_INTEGER due;due.QuadPart=-50000;if(timer)SetWaitableTimer(timer,&due,5,nullptr,nullptr,FALSE);
  AudioConfig c;AudioStatus s;RecordSink record;uint64_t seen=0;Stream mic,app,cable,monitor;bool wantedMic=false,wantedApp=false,wantedCable=false,wantedMonitor=false;auto lastTick=std::chrono::steady_clock::now();auto clippedUntil=lastTick;std::array<float,480> m{},a{},mix{},listen{};
  struct Opened{std::unique_ptr<Stream> stream;HRESULT result;};
  struct Pending{Stream* target;uint64_t version;std::future<Opened> result;};
  std::vector<Pending> pending;std::unordered_map<Stream*,uint64_t> versions;
  auto fail=[&](Stream& stream,QString& text,bool& active,bool& fault,HRESULT hr){stream.close();active=false;fault=true;text=failure(hr);};
  while(!quit){HANDLE events[6];DWORD count=0;events[count++]=wake;if(timer)events[count++]=timer;for(Stream* x:{&mic,&app,&cable,&monitor})if(x->event)events[count++]=x->event;DWORD signaled=WaitForMultipleObjects(count,events,FALSE,timer?100:5);bool tick=!timer||signaled==WAIT_OBJECT_0+1;
   bool changed=false;{std::unique_lock lock(mutex,std::try_to_lock);if(lock.owns_lock()&&seen!=revision){c=requested;record=sink;seen=revision;changed=true;}}
   if(changed){s.generation=c.generation;bool needMic=!c.microphoneId.isEmpty()&&(c.transmit||c.recording||c.monitor);bool needApp=c.share&&(c.transmit||c.recording);bool needCable=c.transmit;bool needMonitor=c.monitor;
    // Keys include only structural settings, so volume/mute edits never restart an endpoint.

    auto update=[&](Stream& stream,bool need,bool& previous,QString& text,bool& active,bool& fault,const QString& key,const QString& id,bool input,const Program* p=nullptr){
     QString old=keys[&stream];if(!need){++versions[&stream];stream.close();active=false;fault=false;text=QStringLiteral("꺼짐");keys[&stream]=key;}else if(!previous||old!=key){stream.close();fault=false;active=false;text=QStringLiteral("연결 중");uint64_t version=++versions[&stream];Program selected=p?*p:Program{};bool processCapture=p!=nullptr;bool stable=c.stable;
      if(pending.size()>=8){fail(stream,text,active,fault,HRESULT_FROM_WIN32(ERROR_BUSY));}
      else try{pending.push_back({&stream,version,std::async(std::launch::async,[id,input,stable,selected,processCapture]{Apartment apartment;auto prepared=std::make_unique<Stream>();HRESULT hr=prepared->open(id,input,stable,processCapture?&selected:nullptr);return Opened{std::move(prepared),hr};})});}catch(...){fail(stream,text,active,fault,E_OUTOFMEMORY);}keys[&stream]=key;}previous=need;};
    const QString mode=c.stable?"S":"N";
    update(mic,needMic,wantedMic,s.microphoneState,s.microphoneActive,s.microphoneFault,c.microphoneId+mode,c.microphoneId,true);
    update(app,needApp,wantedApp,s.programState,s.programActive,s.programFault,QString::number(c.program.pid)+":"+QString::number(c.program.created)+mode,{},true,&c.program);
    update(cable,needCable,wantedCable,s.transmitState,s.transmitActive,s.transmitFault,c.cableId+mode,c.cableId,false);
    update(monitor,needMonitor,wantedMonitor,s.monitorState,s.monitorActive,s.monitorFault,c.monitorId+mode,c.monitorId,false);
    s.formats=QStringLiteral("마이크: %1\n%2\n전송: %3\n모니터: %4\n혼합 5 ms; 피크 보호 lookahead 0 ms; %5 버퍼").arg(mic.description,app.description,cable.description,monitor.description,c.stable?QStringLiteral("안정성 우선"):QStringLiteral("기본"));
   }
   for(auto it=pending.begin();it!=pending.end();){
    if(it->result.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
    Opened opened;try{opened=it->result.get();}catch(...){opened.result=E_OUTOFMEMORY;}Stream* target=it->target;
    if(versions[target]==it->version){
     QString* text=target==&mic?&s.microphoneState:target==&app?&s.programState:target==&cable?&s.transmitState:&s.monitorState;
     bool* active=target==&mic?&s.microphoneActive:target==&app?&s.programActive:target==&cable?&s.transmitActive:&s.monitorActive;
     bool* fault=target==&mic?&s.microphoneFault:target==&app?&s.programFault:target==&cable?&s.transmitFault:&s.monitorFault;
     if(SUCCEEDED(opened.result)){target->adopt(*opened.stream);opened.result=target->start();}
     if(FAILED(opened.result))fail(*target,*text,*active,*fault,opened.result);else{*active=true;*fault=false;*text=QStringLiteral("연결됨");}
     s.formats=QStringLiteral("마이크: %1\n%2\n전송: %3\n모니터: %4\n혼합 5 ms; 피크 보호 lookahead 0 ms; %5 버퍼").arg(mic.description,app.description,cable.description,monitor.description,c.stable?QStringLiteral("안정성 우선"):QStringLiteral("기본"));
    }
    it=pending.erase(it);
   }
   if(app.process&&WaitForSingleObject(app.process,0)!=WAIT_TIMEOUT){app.close();s.programActive=false;s.programFault=true;s.programState=QStringLiteral("선택 프로그램 종료 — 다시 선택하세요. 녹음은 남은 소리/무음으로 계속됩니다.");}
   if(mic.capture){HRESULT hr=mic.read(s.overruns);if(FAILED(hr))fail(mic,s.microphoneState,s.microphoneActive,s.microphoneFault,hr);}
   if(app.capture){HRESULT hr=app.read(s.overruns);if(FAILED(hr))fail(app,s.programState,s.programActive,s.programFault,hr);}
   if(tick){
    auto now=std::chrono::steady_clock::now();auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(now-lastTick).count();
    unsigned blocks=unsigned(std::max<int64_t>(0,elapsed/5000));
    if(blocks>20){s.overruns+=blocks-20;blocks=20;lastTick=now;}else lastTick+=std::chrono::microseconds(blocks*5000);
    for(unsigned block=0;block<blocks;++block){mic.queue.bound(c.stable?1920:960,s.overruns);app.queue.bound(c.stable?1920:960,s.overruns);size_t n=mic.queue.popClocked(m.data(),240,c.stable?1440:480);if(s.microphoneActive&&n<240)++s.underruns;n=app.queue.popClocked(a.data(),240,c.stable?1440:480);if(s.programActive&&n<240)++s.underruns;
    s.microphonePeak=peak(m.data(),480);s.programPeak=peak(a.data(),480);s.clipped=now<clippedUntil;
    for(size_t i=0;i<480;++i){float mv=std::isfinite(m[i])?m[i]:0;float av=std::isfinite(a[i])?a[i]:0;float sum=(c.muted?0:mv*c.microphoneGain)+(c.share?av*c.programGain:0);if(std::abs(sum)>.98f){s.clipped=true;clippedUntil=now+std::chrono::seconds(1);}mix[i]=std::clamp(sum,-.98f,.98f);listen[i]=std::clamp(mv*c.monitorGain,-.98f,.98f);}
    s.mixPeak=peak(mix.data(),480);if(s.transmitActive)cable.queue.push(mix.data(),240,s.overruns);if(s.monitorActive)monitor.queue.push(listen.data(),240,s.overruns);if(c.recording&&record)record(mix.data(),240);
    }
   }
   if(cable.render){HRESULT hr=cable.write(s.underruns,s.overruns,c.stable);if(FAILED(hr))fail(cable,s.transmitState,s.transmitActive,s.transmitFault,hr);}
   if(monitor.render){HRESULT hr=monitor.write(s.underruns,s.overruns,c.stable);if(FAILED(hr))fail(monitor,s.monitorState,s.monitorActive,s.monitorFault,hr);}
   s.queueMilliseconds=double(std::max(mic.queue.count,app.queue.count)+std::max(cable.queue.count,monitor.queue.count))/48.0+5.0+double(std::max(cable.capacity,monitor.capacity))/48.0;
   {std::unique_lock lock(mutex,std::try_to_lock);if(lock.owns_lock())state=s;} appliedRevision.store(seen);SetEvent(applied);
  }mic.close();app.close();cable.close();monitor.close();if(timer){CancelWaitableTimer(timer);CloseHandle(timer);}if(priority)AvRevertMmThreadCharacteristics(priority);
 }
 std::unordered_map<Stream*,QString> keys;
};
AudioEngine::AudioEngine():d(std::make_unique<Impl>()){}AudioEngine::~AudioEngine()=default;
std::vector<Device> AudioEngine::inputDevices(){return devices(eCapture);}std::vector<Device> AudioEngine::outputDevices(){return devices(eRender);}
std::vector<Program> AudioEngine::programs(){
 auto tree=parents();std::vector<Program> result;QFileIconProvider icons;
 struct Windows {std::vector<DWORD> ids;};Windows windows;EnumWindows([](HWND hwnd,LPARAM data)->BOOL{if(IsWindowVisible(hwnd)&&GetWindowTextLengthW(hwnd)>0){DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);reinterpret_cast<Windows*>(data)->ids.push_back(pid);}return TRUE;},reinterpret_cast<LPARAM>(&windows));
 for(DWORD pid:windows.ids){if(unsafe(pid,tree))continue;HANDLE h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);if(!h)continue;std::array<wchar_t,32768> path{};DWORD length=DWORD(path.size());if(!QueryFullProcessImageNameW(h,0,path.data(),&length)){CloseHandle(h);continue;}Program p;p.pid=pid;p.created=creation(h);CloseHandle(h);p.path=QString::fromWCharArray(path.data(),int(length));
  // Select the oldest same-executable ancestor to include a browser's renderer children.
  for(size_t n=0;n<tree.size();++n){auto parent=tree.find(p.pid);if(parent==tree.end()||!parent->second||parent->second==p.pid||unsafe(parent->second,tree))break;HANDLE ph=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,parent->second);if(!ph)break;length=DWORD(path.size());bool same=QueryFullProcessImageNameW(ph,0,path.data(),&length)&&QString::fromWCharArray(path.data(),int(length)).compare(p.path,Qt::CaseInsensitive)==0;uint64_t created=creation(ph);CloseHandle(ph);if(!same||created>p.created)break;p.pid=parent->second;p.created=created;}
  if(std::any_of(result.begin(),result.end(),[&](const Program& v){return v.pid==p.pid;}))continue;p.name=QFileInfo(p.path).completeBaseName();p.icon=icons.icon(QFileInfo(p.path));result.push_back(p);
 }std::sort(result.begin(),result.end(),[](const Program& a,const Program& b){return a.name.compare(b.name,Qt::CaseInsensitive)<0;});return result;
}
bool AudioEngine::configure(const AudioConfig& config,QString& error){
 error.clear();bool active=config.transmit||config.recording;bool source=(!config.microphoneId.isEmpty()&&!config.muted&&config.microphoneGain>0)||(config.share&&config.program.pid&&config.programGain>0);
 AudioConfig previous;{std::lock_guard lock(d->mutex);previous=d->requested;}
 if(active&&(!previous.transmit&&config.transmit||!previous.recording&&config.recording)&&!source){error=QStringLiteral("활성 소스가 없습니다. 마이크를 선택하고 음소거를 해제하거나 프로그램 공유를 켜세요.");return false;}
 if(config.share&&(!previous.share||previous.program.pid!=config.program.pid||previous.program.created!=config.program.created)&&!validProgram(config.program)){error=QStringLiteral("선택 프로그램이 종료되었거나 앱 자신/상위 프로세스여서 안전하게 분리할 수 없습니다. 프로그램을 다시 선택하세요.");return false;}
 if(config.transmit&&config.cableId.isEmpty()){error=QStringLiteral("VB-CABLE이 없습니다. 공식 드라이버 설치 후 장치를 새로 고치세요.");return false;}
 if(config.monitor&&(config.monitorId.isEmpty()||config.microphoneId.isEmpty())){error=QStringLiteral("내 목소리 듣기에는 마이크와 실제 이어폰을 선택해야 합니다.");return false;}
 if((config.monitor&&(config.monitorId!=previous.monitorId||!previous.monitor))||(config.transmit&&(config.cableId!=previous.cableId||!previous.transmit))){auto outputs=outputDevices();auto exists=[&](const QString& id,bool cable){return std::any_of(outputs.begin(),outputs.end(),[&](const Device& x){return x.id==id&&x.cable==cable;});};if(config.monitor&&!exists(config.monitorId,false)){error=QStringLiteral("모니터링에는 연결된 실제 이어폰만 사용할 수 있습니다.");return false;}if(config.transmit&&!exists(config.cableId,true)){error=QStringLiteral("연결된 가상 케이블 재생 장치를 선택하세요.");return false;}}
 if(!config.microphoneId.isEmpty()&&(config.microphoneId!=previous.microphoneId||(!previous.transmit&&!previous.recording&&!previous.monitor)&&(config.transmit||config.recording||config.monitor))){auto inputs=inputDevices();if(std::none_of(inputs.begin(),inputs.end(),[&](const Device& x){return x.id==config.microphoneId;})){error=QStringLiteral("선택 마이크가 연결되어 있지 않습니다. 새로 고침 후 다시 선택하세요.");return false;}}
 {std::lock_guard lock(d->mutex);d->requested=config;d->requested.microphoneGain=std::clamp(config.microphoneGain,0.f,2.f);d->requested.programGain=std::clamp(config.programGain,0.f,2.f);d->requested.monitorGain=std::clamp(config.monitorGain,0.f,2.f);++d->revision;}SetEvent(d->wake);return true;
}
void AudioEngine::setRecordSink(RecordSink sink){std::lock_guard lock(d->mutex);d->sink=std::move(sink);++d->revision;SetEvent(d->wake);}
AudioStatus AudioEngine::status()const{std::lock_guard lock(d->mutex);return d->state;}
void AudioEngine::stop(){uint64_t revision;{std::lock_guard lock(d->mutex);d->requested.transmit=d->requested.monitor=d->requested.recording=d->requested.share=false;revision=++d->revision;}SetEvent(d->wake);while(d->appliedRevision.load()<revision&&!d->quit)WaitForSingleObject(d->applied,100);}
}




